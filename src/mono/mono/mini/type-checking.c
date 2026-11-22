/**
 * \file
 */

#include <config.h>
#include <mono/utils/mono-compiler.h>

#ifndef DISABLE_JIT

#include "mini.h"
#include "ir-emit.h"
#include <mono/metadata/abi-details.h>
#include <mono/metadata/class-abi-details.h>


#define is_complex_isinst(klass) (mono_class_is_interface (klass) || m_class_get_rank (klass) || mono_class_is_nullable (klass) || mono_class_is_sealed (klass) || m_class_get_byval_arg (klass)->type == MONO_TYPE_VAR || m_class_get_byval_arg (klass)->type == MONO_TYPE_MVAR)

static int
get_castclass_cache_idx (MonoCompile *cfg)
{
	/* Each CASTCLASS_CACHE patch needs a unique index which identifies the call site */
	cfg->castclass_cache_index ++;
	return (cfg->method_index << 16) | cfg->castclass_cache_index;
}

static void
emit_cached_check_args (MonoCompile *cfg, MonoInst *obj, MonoClass *klass, int context_used, MonoInst *args[3])
{
	args [0] = obj;

	if (context_used) {
		MonoInst *cache_ins;

		cache_ins = mini_emit_get_rgctx_klass (cfg, context_used, klass, MONO_RGCTX_INFO_CAST_CACHE);

		/* klass - it's the second element of the cache entry*/
		EMIT_NEW_LOAD_MEMBASE (cfg, args [1], OP_LOAD_MEMBASE, alloc_preg (cfg), cache_ins->dreg, TARGET_SIZEOF_VOID_P);

		args [2] = cache_ins; /* cache */
	} else {
		int idx;

		g_assert (klass);
		EMIT_NEW_CLASSCONST (cfg, args [1], klass); /* klass */

		idx = get_castclass_cache_idx (cfg); /* inline cache*/
		args [2] = mini_emit_runtime_constant (cfg, MONO_PATCH_INFO_CASTCLASS_CACHE, GINT_TO_POINTER (idx));
	}
}

static MonoInst*
emit_isinst_with_cache (MonoCompile *cfg, MonoInst *obj, MonoClass *klass, int context_used)
{
	MonoInst *args [3];
	MonoMethod *mono_isinst = mono_marshal_get_isinst_with_cache ();

	emit_cached_check_args (cfg, obj, klass, context_used, args);
	return mono_emit_method_call (cfg, mono_isinst, args, NULL);
}

static MonoInst*
emit_castclass_with_cache_no_details (MonoCompile *cfg, MonoInst *obj, MonoClass *klass, int context_used)
{
	MonoInst *args [3];
	MonoMethod *mono_castclass = mono_marshal_get_castclass_with_cache ();
	MonoInst *res;

	emit_cached_check_args (cfg, obj, klass, context_used, args);

	res = mono_emit_method_call (cfg, mono_castclass, args, NULL);

	return res;
}

static MonoInst*
emit_castclass_with_cache (MonoCompile *cfg, MonoInst *obj, MonoClass *klass, int context_used)
{
	MonoInst *args [3];
	MonoMethod *mono_castclass = mono_marshal_get_castclass_with_cache ();
	MonoInst *res;

	emit_cached_check_args (cfg, obj, klass, context_used, args);

	mini_save_cast_details (cfg, klass, args [0]->dreg, TRUE);
	res = mono_emit_method_call (cfg, mono_castclass, args, NULL);
	mini_reset_cast_details (cfg);

	return res;
}

static void
mini_emit_class_check_inst (MonoCompile *cfg, int klass_reg, MonoClass *klass, MonoInst *klass_inst)
{
	if (klass_inst) {
		MONO_EMIT_NEW_BIALU (cfg, OP_COMPARE, -1, klass_reg, klass_inst->dreg);
	} else {
		MonoInst *ins = mini_emit_runtime_constant (cfg, MONO_PATCH_INFO_CLASS, klass);
		MONO_EMIT_NEW_BIALU (cfg, OP_COMPARE, -1, klass_reg, ins->dreg);
	}
	MONO_EMIT_NEW_COND_EXC (cfg, NE_UN, "InvalidCastException");
}


#ifdef HOST_BROWSER
/* MONO_WASM_JIT_CLASS_RECORD (metadata/object.c, plan abstract-hatching-stearns B2). */
extern int mono_wasm_class_record_bits (void);
extern int mono_wasm_class_record_offset (void);
extern gboolean mono_wasm_class_record_had_provisional_vtable (MonoClass *klass);
static gboolean wj_cast_target_unfinished (MonoClass *klass);

/* Bit 1: can the object's display decide a check against KLASS? A class of depth 1..8 (not an interface or array),
 * with no generic context, not an unfinished TypeBuilder (CAST_UNFINISHED's case), once the first record exists. */
static gboolean
wj_display_ok (MonoCompile *cfg, MonoClass *klass, MonoInst *klass_inst)
{
	if (!COMPILE_WASM (cfg) || klass_inst || !(mono_wasm_class_record_bits () & 1) || !mono_wasm_class_record_offset ())
		return FALSE;
	if (mono_class_is_interface (klass) || m_class_get_rank (klass) || wj_cast_target_unfinished (klass))
		return FALSE;
	mono_class_setup_supertypes (klass);
	return m_class_get_idepth (klass) >= 1 && m_class_get_idepth (klass) <= 8;
}

/* compare (display [idepth - 1] of the object whose vtable is VTABLE_REG, KLASS): the record is below the vtable, so the
 * negative part of the offset is an add (the wasm memarg offset is unsigned) */
extern gint32 mono_wasm_jit_classrec_bad;
static void
wj_emit_display_compare (MonoCompile *cfg, int vtable_reg, MonoClass *klass)
{
	int rec = alloc_preg (cfg), disp = alloc_preg (cfg), d = m_class_get_idepth (klass);
	MONO_EMIT_NEW_BIALU_IMM (cfg, OP_PADD_IMM, rec, vtable_reg, mono_wasm_class_record_offset ());
	MONO_EMIT_NEW_LOAD_MEMBASE (cfg, disp, rec, (d - 1) * TARGET_SIZEOF_VOID_P);
	if (mono_wasm_class_record_bits () & 16) {
		/* verify: the old answer -- vt->klass->idepth >= d && vt->klass->supertypes [d - 1] == klass -- against the
		 * display's, branch-free (a supertypes read past the class's own table is masked by the depth test), every
		 * disagreement added to mono_wasm_jit_classrec_bad (`[wasm-jit classrec] ... verify_bad=`, must read 0) */
		int kr = alloc_preg (cfg), idep = alloc_ireg (cfg), st = alloc_preg (cfg), sty = alloc_preg (cfg);
		int lt = alloc_ireg (cfg), deep = alloc_ireg (cfg), same = alloc_ireg (cfg), old_ok = alloc_ireg (cfg);
		int dok = alloc_ireg (cfg), bad = alloc_ireg (cfg), a = alloc_preg (cfg), v = alloc_ireg (cfg);
		MONO_EMIT_NEW_LOAD_MEMBASE (cfg, kr, vtable_reg, MONO_STRUCT_OFFSET (MonoVTable, klass));
		MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADU2_MEMBASE, idep, kr, GINTPTR_TO_TMREG (m_class_offsetof_idepth ()));
		MONO_EMIT_NEW_LOAD_MEMBASE (cfg, st, kr, GINTPTR_TO_TMREG (m_class_offsetof_supertypes ()));
		MONO_EMIT_NEW_LOAD_MEMBASE (cfg, sty, st, (d - 1) * TARGET_SIZEOF_VOID_P);
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ICOMPARE_IMM, -1, idep, d);
		MONO_EMIT_NEW_UNALU (cfg, OP_ICLT_UN, lt, -1);
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_IXOR_IMM, deep, lt, 1);
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ICOMPARE_IMM, -1, sty, (gsize) klass);
		MONO_EMIT_NEW_UNALU (cfg, OP_ICEQ, same, -1);
		MONO_EMIT_NEW_BIALU (cfg, OP_IAND, old_ok, deep, same);
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ICOMPARE_IMM, -1, disp, (gsize) klass);
		MONO_EMIT_NEW_UNALU (cfg, OP_ICEQ, dok, -1);
		MONO_EMIT_NEW_BIALU (cfg, OP_IXOR, bad, old_ok, dok);
		MONO_EMIT_NEW_PCONST (cfg, a, (gpointer) &mono_wasm_jit_classrec_bad);
		MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADI4_MEMBASE, v, a, 0);
		MONO_EMIT_NEW_BIALU (cfg, OP_IADD, v, v, bad);
		MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STOREI4_MEMBASE_REG, a, 0, v);
	}
	MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, disp, (gsize) klass);
	mono_wasm_jit_counters [WJC_CLASSREC_DISPLAY_SITES]++;
}

/* Bit 2: KLASS's runtime vtable, when a sealed check may compare the object's vtable with it */
static MonoVTable *
wj_sealed_vtable (MonoCompile *cfg, MonoClass *klass)
{
	MonoVTable *vt;
	if (!COMPILE_WASM (cfg) || !(mono_wasm_class_record_bits () & 2) || m_class_get_rank (klass) || wj_cast_target_unfinished (klass))
		return NULL;
	if (mono_wasm_class_record_had_provisional_vtable (klass))
		return NULL;   /* it may have two live vtables (metadata/object.c) */
	vt = mono_class_try_get_vtable (klass);
	if (vt)
		mono_wasm_jit_counters [WJC_CLASSREC_SEALED_SITES]++;
	return vt;
}
#endif

static void
mini_emit_isninst_cast_inst (MonoCompile *cfg, int klass_reg, MonoClass *klass, MonoInst *klass_ins, MonoBasicBlock *false_target, MonoBasicBlock *true_target)
{
	int idepth_reg = alloc_preg (cfg);
	int stypes_reg = alloc_preg (cfg);
	int stype = alloc_preg (cfg);

	mono_class_setup_supertypes (klass);

	if (m_class_get_idepth (klass) > MONO_DEFAULT_SUPERTABLE_SIZE) {
		MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADU2_MEMBASE, idepth_reg, klass_reg, GINTPTR_TO_TMREG (m_class_offsetof_idepth ()));
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, idepth_reg, m_class_get_idepth (klass));
		MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_PBLT_UN, false_target);
	}
	MONO_EMIT_NEW_LOAD_MEMBASE (cfg, stypes_reg, klass_reg, GINTPTR_TO_TMREG (m_class_offsetof_supertypes ()));
	MONO_EMIT_NEW_LOAD_MEMBASE (cfg, stype, stypes_reg, ((m_class_get_idepth (klass) - 1) * TARGET_SIZEOF_VOID_P));
	if (klass_ins) {
		MONO_EMIT_NEW_BIALU (cfg, OP_COMPARE, -1, stype, klass_ins->dreg);
	} else if (cfg->compile_aot) {
		int const_reg = alloc_preg (cfg);
		MONO_EMIT_NEW_CLASSCONST (cfg, const_reg, klass);
		MONO_EMIT_NEW_BIALU (cfg, OP_COMPARE, -1, stype, const_reg);
	} else {
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, stype, (gsize)klass);
	}
	MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_PBEQ, true_target);
}


static void
mini_emit_interface_bitmap_check (MonoCompile *cfg, int intf_bit_reg, int base_reg, intptr_t offset, MonoClass *klass)
{
	int ibitmap_reg = alloc_preg (cfg);

	if (mono_opt_compressed_interface_bitmap) {
		MonoInst *args [2];
		MonoInst *res, *ins;
		NEW_LOAD_MEMBASE (cfg, ins, OP_LOAD_MEMBASE, ibitmap_reg, base_reg, (target_mgreg_t)offset);
		MONO_ADD_INS (cfg->cbb, ins);
		args [0] = ins;
		args [1] = mini_emit_runtime_constant (cfg, MONO_PATCH_INFO_IID, klass);
		res = mono_emit_jit_icall (cfg, mono_class_interface_match_compressed, args);
		MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, intf_bit_reg, res->dreg);
	} else {
		int ibitmap_byte_reg = alloc_preg (cfg);

		MONO_EMIT_NEW_LOAD_MEMBASE (cfg, ibitmap_reg, base_reg, GINTPTR_TO_TMREG (offset));

		if (cfg->compile_aot) {
			int iid_reg = alloc_preg (cfg);
			int shifted_iid_reg = alloc_preg (cfg);
			int ibitmap_byte_address_reg = alloc_preg (cfg);
			int masked_iid_reg = alloc_preg (cfg);
			int iid_one_bit_reg = alloc_preg (cfg);
			int iid_bit_reg = alloc_preg (cfg);
			MONO_EMIT_NEW_AOTCONST (cfg, iid_reg, klass, MONO_PATCH_INFO_IID);
			MONO_EMIT_NEW_BIALU_IMM (cfg, OP_SHR_IMM, shifted_iid_reg, iid_reg, 3);
			MONO_EMIT_NEW_BIALU (cfg, OP_PADD, ibitmap_byte_address_reg, ibitmap_reg, shifted_iid_reg);
			MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADU1_MEMBASE, ibitmap_byte_reg, ibitmap_byte_address_reg, 0);
			MONO_EMIT_NEW_BIALU_IMM (cfg, OP_IAND_IMM, masked_iid_reg, iid_reg, 7);
			MONO_EMIT_NEW_ICONST (cfg, iid_one_bit_reg, 1);
			MONO_EMIT_NEW_BIALU (cfg, OP_ISHL, iid_bit_reg, iid_one_bit_reg, masked_iid_reg);
			MONO_EMIT_NEW_BIALU (cfg, OP_IAND, intf_bit_reg, ibitmap_byte_reg, iid_bit_reg);
		} else {
			MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADI1_MEMBASE, ibitmap_byte_reg, ibitmap_reg, m_class_get_interface_id (klass) >> 3);
			MONO_EMIT_NEW_BIALU_IMM (cfg, OP_AND_IMM, intf_bit_reg, ibitmap_byte_reg, ((target_mgreg_t)1) << (m_class_get_interface_id (klass) & 7));
	    }
	}
}

/*
 * Emit code which loads into "intf_bit_reg" a nonzero value if the MonoClass
 * stored in "klass_reg" implements the interface "klass".
 */
static void
mini_emit_load_intf_bit_reg_class (MonoCompile *cfg, int intf_bit_reg, int klass_reg, MonoClass *klass)
{
	mini_emit_interface_bitmap_check (cfg, intf_bit_reg, klass_reg, m_class_offsetof_interface_bitmap (), klass);
}

/*
 * Emit code which loads into "intf_bit_reg" a nonzero value if the MonoVTable
 * stored in "vtable_reg" implements the interface "klass".
 */
static void
mini_emit_load_intf_bit_reg_vtable (MonoCompile *cfg, int intf_bit_reg, int vtable_reg, MonoClass *klass)
{
	mini_emit_interface_bitmap_check (cfg, intf_bit_reg, vtable_reg, MONO_STRUCT_OFFSET (MonoVTable, interface_bitmap), klass);
}

/*
 * Emit code which checks whenever the interface id of @klass is smaller than
 * than the value given by max_iid_reg.
*/
static void
mini_emit_max_iid_check (MonoCompile *cfg, int max_iid_reg, MonoClass *klass,
						 MonoBasicBlock *false_target)
{
	if (cfg->compile_aot) {
		int iid_reg = alloc_preg (cfg);
		MONO_EMIT_NEW_AOTCONST (cfg, iid_reg, klass, MONO_PATCH_INFO_IID);
		MONO_EMIT_NEW_BIALU (cfg, OP_COMPARE, -1, max_iid_reg, iid_reg);
	}
	else
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, max_iid_reg, m_class_get_interface_id (klass));
	if (false_target)
		MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_PBLT_UN, false_target);
	else
		MONO_EMIT_NEW_COND_EXC (cfg, LT_UN, "InvalidCastException");
}

/* Same as above, but obtains max_iid from a vtable */
static void
mini_emit_max_iid_check_vtable (MonoCompile *cfg, int vtable_reg, MonoClass *klass,
								 MonoBasicBlock *false_target)
{
	int max_iid_reg = alloc_preg (cfg);

	MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADU4_MEMBASE, max_iid_reg, vtable_reg, MONO_STRUCT_OFFSET (MonoVTable, max_interface_id));
	mini_emit_max_iid_check (cfg, max_iid_reg, klass, false_target);
}

/* Same as above, but obtains max_iid from a klass */
static void
mini_emit_max_iid_check_class (MonoCompile *cfg, int klass_reg, MonoClass *klass,
								 MonoBasicBlock *false_target)
{
	int max_iid_reg = alloc_preg (cfg);

	MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADU4_MEMBASE, max_iid_reg, klass_reg, GINTPTR_TO_TMREG (m_class_offsetof_max_interface_id ()));
	mini_emit_max_iid_check (cfg, max_iid_reg, klass, false_target);
}

static void
mini_emit_class_check_branch (MonoCompile *cfg, int klass_reg, MonoClass *klass, int branch_op, MonoBasicBlock *target)
{
	if (cfg->compile_aot) {
		int const_reg = alloc_preg (cfg);
		MONO_EMIT_NEW_CLASSCONST (cfg, const_reg, klass);
		MONO_EMIT_NEW_BIALU (cfg, OP_COMPARE, -1, klass_reg, const_reg);
	} else {
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, klass_reg, (gsize)klass);
	}
	MONO_EMIT_NEW_BRANCH_BLOCK (cfg, branch_op, target);
}


static void
mini_emit_isninst_cast (MonoCompile *cfg, int klass_reg, MonoClass *klass, MonoBasicBlock *false_target, MonoBasicBlock *true_target)
{
	mini_emit_isninst_cast_inst (cfg, klass_reg, klass, NULL, false_target, true_target);
}

static void
mini_emit_iface_cast (MonoCompile *cfg, int vtable_reg, MonoClass *klass, MonoBasicBlock *false_target, MonoBasicBlock *true_target)
{
	int intf_reg = alloc_preg (cfg);

	mini_emit_max_iid_check_vtable (cfg, vtable_reg, klass, false_target);
	mini_emit_load_intf_bit_reg_vtable (cfg, intf_reg, vtable_reg, klass);
	MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, intf_reg, 0);
	if (true_target)
		MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_PBNE_UN, true_target);
	else
		MONO_EMIT_NEW_COND_EXC (cfg, EQ, "InvalidCastException");
}

/*
 * Variant of the above that takes a register to the class, not the vtable.
 */
static void
mini_emit_iface_class_cast (MonoCompile *cfg, int klass_reg, MonoClass *klass, MonoBasicBlock *false_target, MonoBasicBlock *true_target)
{
	int intf_bit_reg = alloc_preg (cfg);

	mini_emit_max_iid_check_class (cfg, klass_reg, klass, false_target);
	mini_emit_load_intf_bit_reg_class (cfg, intf_bit_reg, klass_reg, klass);
	MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, intf_bit_reg, 0);
	if (true_target)
		MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_PBNE_UN, true_target);
	else
		MONO_EMIT_NEW_COND_EXC (cfg, EQ, "InvalidCastException");
}


static void
mini_emit_castclass (MonoCompile *cfg, int obj_reg, int klass_reg, MonoClass *klass, MonoBasicBlock *object_is_null);

static void
mini_emit_castclass_inst (MonoCompile *cfg, int obj_reg, int klass_reg, MonoClass *klass, MonoInst *klass_inst, MonoBasicBlock *object_is_null)
{
	if (m_class_get_rank (klass)) {
		int rank_reg = alloc_preg (cfg);
		int eclass_reg = alloc_preg (cfg);

		g_assert (!klass_inst);

		MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADU1_MEMBASE, rank_reg, klass_reg, GINTPTR_TO_TMREG (m_class_offsetof_rank ()));
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, rank_reg, m_class_get_rank (klass));
		MONO_EMIT_NEW_COND_EXC (cfg, NE_UN, "InvalidCastException");

		//		MONO_EMIT_NEW_LOAD_MEMBASE (cfg, klass_reg, vtable_reg, MONO_STRUCT_OFFSET (MonoVTable, klass));
		MONO_EMIT_NEW_LOAD_MEMBASE (cfg, eclass_reg, klass_reg, GINTPTR_TO_TMREG (m_class_offsetof_cast_class ()));
		if (m_class_is_array_special_interface (m_class_get_cast_class (klass))) {
			MonoInst *src;

			MONO_INST_NEW (cfg, src, OP_LOCAL);
			src->dreg = obj_reg;
			emit_castclass_with_cache_no_details (cfg, src, klass, 0);
		} else if (m_class_get_cast_class (klass) == mono_defaults.object_class) {
			int parent_reg = alloc_preg (cfg);
			MONO_EMIT_NEW_LOAD_MEMBASE (cfg, parent_reg, eclass_reg, GINTPTR_TO_TMREG (m_class_offsetof_parent ()));
			mini_emit_class_check_branch (cfg, parent_reg, m_class_get_parent (mono_defaults.enum_class), OP_PBNE_UN, object_is_null);
			mini_emit_class_check (cfg, eclass_reg, mono_defaults.enum_class);
		} else if (m_class_get_cast_class (klass) == m_class_get_parent (mono_defaults.enum_class)) {
			mini_emit_class_check_branch (cfg, eclass_reg, m_class_get_parent (mono_defaults.enum_class), OP_PBEQ, object_is_null);
			mini_emit_class_check (cfg, eclass_reg, mono_defaults.enum_class);
		} else if (m_class_get_cast_class (klass) == mono_defaults.enum_class) {
			mini_emit_class_check (cfg, eclass_reg, mono_defaults.enum_class);
		} else if (mono_class_is_interface (m_class_get_cast_class (klass))) {
			mini_emit_iface_class_cast (cfg, eclass_reg, m_class_get_cast_class (klass), NULL, NULL);
		} else {
			// Pass -1 as obj_reg to skip the check below for arrays of arrays
			mini_emit_castclass (cfg, -1, eclass_reg, m_class_get_cast_class (klass), object_is_null);
		}

		if ((m_class_get_rank (klass) == 1) && (m_class_get_byval_arg (klass)->type == MONO_TYPE_SZARRAY) && (obj_reg != -1)) {
			/* Check that the object is a vector too */
			int bounds_reg = alloc_preg (cfg);
			MONO_EMIT_NEW_LOAD_MEMBASE (cfg, bounds_reg, obj_reg, MONO_STRUCT_OFFSET (MonoArray, bounds));
			MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, bounds_reg, 0);
			MONO_EMIT_NEW_COND_EXC (cfg, NE_UN, "InvalidCastException");
		}
	} else {
		int idepth_reg = alloc_preg (cfg);
		int stypes_reg = alloc_preg (cfg);
		int stype = alloc_preg (cfg);

		mono_class_setup_supertypes (klass);

		if (m_class_get_idepth (klass) > MONO_DEFAULT_SUPERTABLE_SIZE) {
			MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADU2_MEMBASE, idepth_reg, klass_reg, GINTPTR_TO_TMREG (m_class_offsetof_idepth ()));
			MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, idepth_reg, m_class_get_idepth (klass));
			MONO_EMIT_NEW_COND_EXC (cfg, LT_UN, "InvalidCastException");
		}
		MONO_EMIT_NEW_LOAD_MEMBASE (cfg, stypes_reg, klass_reg, GINTPTR_TO_TMREG (m_class_offsetof_supertypes ()));
		MONO_EMIT_NEW_LOAD_MEMBASE (cfg, stype, stypes_reg, ((m_class_get_idepth (klass) - 1) * TARGET_SIZEOF_VOID_P));
		mini_emit_class_check_inst (cfg, stype, klass, klass_inst);
	}
}

static void
mini_emit_castclass (MonoCompile *cfg, int obj_reg, int klass_reg, MonoClass *klass, MonoBasicBlock *object_is_null)
{
	mini_emit_castclass_inst (cfg, obj_reg, klass_reg, klass, NULL, object_is_null);
}

static void
emit_special_array_iface_check (MonoCompile *cfg, MonoInst *src, MonoClass* klass, int vtable_reg, MonoBasicBlock *not_an_array, MonoBasicBlock *true_bb, int context_used)
{
	int rank_reg;

	if (!m_class_is_array_special_interface (klass))
		return;

	rank_reg = alloc_ireg (cfg);

	MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADU1_MEMBASE, rank_reg, vtable_reg, MONO_STRUCT_OFFSET (MonoVTable, rank));
	MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, rank_reg, 1);
	if (not_an_array)
		MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_IBNE_UN, not_an_array);
	else
		MONO_EMIT_NEW_COND_EXC (cfg, NE_UN, "InvalidCastException");

	emit_castclass_with_cache_no_details (cfg, src, klass, context_used);
	MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_BR, true_bb);
}

/*
 * Returns NULL and set the cfg exception on error.
 */
static MonoInst*
handle_castclass (MonoCompile *cfg, MonoClass *klass, MonoInst *src, int context_used)
{
	MonoBasicBlock *is_null_bb;
	int obj_reg = src->dreg;
	MonoInst *klass_inst = NULL;

	if (MONO_INS_IS_PCONST_NULL (src))
		return src;

	if (context_used) {

		if (is_complex_isinst (klass))
			return emit_castclass_with_cache (cfg, src, klass, context_used);

		klass_inst = mini_emit_get_rgctx_klass (cfg, context_used, klass, MONO_RGCTX_INFO_KLASS);
	}

	NEW_BBLOCK (cfg, is_null_bb);

	MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, obj_reg, 0);
	MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_PBEQ, is_null_bb);

	mini_save_cast_details (cfg, klass, obj_reg, FALSE);

	if (mono_class_is_interface (klass)) {
		int tmp_reg = alloc_preg (cfg);
		MonoBasicBlock *interface_fail_bb = NULL;

		MONO_EMIT_NEW_LOAD_MEMBASE (cfg, tmp_reg, obj_reg, MONO_STRUCT_OFFSET (MonoObject, vtable));

		if (m_class_is_array_special_interface (klass)) {
			NEW_BBLOCK (cfg, interface_fail_bb);
			mini_emit_iface_cast (cfg, tmp_reg, klass, interface_fail_bb, is_null_bb);
			// iface bitmap check failed
			MONO_START_BB (cfg, interface_fail_bb);

			//Check if it's a rank zero array and emit fallback casting
			emit_special_array_iface_check (cfg, src, klass, tmp_reg, NULL, is_null_bb, context_used);
		} else {
			mini_emit_iface_cast (cfg, tmp_reg, klass, NULL, NULL);
			MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_BR, is_null_bb);
		}
	} else {
		int vtable_reg = alloc_preg (cfg);
		int klass_reg = alloc_preg (cfg);

		MONO_EMIT_NEW_LOAD_MEMBASE (cfg, vtable_reg, obj_reg, MONO_STRUCT_OFFSET (MonoObject, vtable));

		if (!m_class_get_rank (klass) && mono_class_is_sealed (klass)) {
#ifdef HOST_BROWSER
			MonoVTable *svt = cfg->compile_aot ? NULL : wj_sealed_vtable (cfg, klass);
#else
			MonoVTable *svt = NULL;
#endif
			if (cfg->compile_aot) {
				MonoInst *vtable_ins;
				EMIT_NEW_AOTCONST (cfg, vtable_ins, MONO_PATCH_INFO_VTABLE, klass);
				MONO_EMIT_NEW_BIALU (cfg, OP_COMPARE, -1, vtable_reg, vtable_ins->dreg);
			} else if (svt) {
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, vtable_reg, (gsize) svt);   /* B2 bit 2 */
			} else {
				MONO_EMIT_NEW_LOAD_MEMBASE (cfg, klass_reg, vtable_reg, MONO_STRUCT_OFFSET (MonoVTable, klass));
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, klass_reg, (gsize)klass);
			}
			MONO_EMIT_NEW_COND_EXC (cfg, NE_UN, "InvalidCastException");
		}
#ifdef HOST_BROWSER
		else if (wj_display_ok (cfg, klass, klass_inst)) {
			wj_emit_display_compare (cfg, vtable_reg, klass);   /* B2 bit 1 */
			MONO_EMIT_NEW_COND_EXC (cfg, NE_UN, "InvalidCastException");
		}
#endif
		else {
			MONO_EMIT_NEW_LOAD_MEMBASE (cfg, klass_reg, vtable_reg, MONO_STRUCT_OFFSET (MonoVTable, klass));
			mini_emit_castclass_inst (cfg, obj_reg, klass_reg, klass, klass_inst, is_null_bb);
		}
	}

	MONO_START_BB (cfg, is_null_bb);

	mini_reset_cast_details (cfg);

	return src;
}

/*
 * Returns NULL and set the cfg exception on error.
 */
static MonoInst*
handle_isinst (MonoCompile *cfg, MonoClass *klass, MonoInst *src, int context_used)
{
	MonoInst *ins;
	MonoBasicBlock *is_null_bb, *false_bb, *end_bb;
	int obj_reg = src->dreg;
	int vtable_reg = alloc_preg (cfg);
	int res_reg = alloc_ireg_ref (cfg);
	MonoInst *klass_inst = NULL;

	if (context_used) {
		if(is_complex_isinst (klass))
			return emit_isinst_with_cache (cfg, src, klass, context_used);

		klass_inst = mini_emit_get_rgctx_klass (cfg, context_used, klass, MONO_RGCTX_INFO_KLASS);
	}

	NEW_BBLOCK (cfg, is_null_bb);
	NEW_BBLOCK (cfg, false_bb);
	NEW_BBLOCK (cfg, end_bb);

	/* Do the assignment at the beginning, so the other assignment can be if converted */
	EMIT_NEW_UNALU (cfg, ins, OP_MOVE, res_reg, obj_reg);
	ins->type = STACK_OBJ;
	ins->klass = klass;

	MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, obj_reg, 0);
	MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_IBEQ, is_null_bb);

	MONO_EMIT_NEW_LOAD_MEMBASE (cfg, vtable_reg, obj_reg, MONO_STRUCT_OFFSET (MonoObject, vtable));

	if (mono_class_is_interface (klass)) {
		MonoBasicBlock *interface_fail_bb;

		NEW_BBLOCK (cfg, interface_fail_bb);

		mini_emit_iface_cast (cfg, vtable_reg, klass, interface_fail_bb, is_null_bb);
		MONO_START_BB (cfg, interface_fail_bb);

		if (m_class_is_array_special_interface (klass)) {
			MonoBasicBlock *not_an_array;
			MonoInst *move;
			int rank_reg = alloc_ireg (cfg);

			NEW_BBLOCK (cfg, not_an_array);
			MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADU1_MEMBASE, rank_reg, vtable_reg, MONO_STRUCT_OFFSET (MonoVTable, rank));
			MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, rank_reg, 1);
			MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_IBNE_UN, not_an_array);

			MonoInst *res_inst = emit_isinst_with_cache (cfg, src, klass, context_used);
			EMIT_NEW_UNALU (cfg, move, OP_MOVE, res_reg, res_inst->dreg);
			MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_BR, end_bb);

			MONO_START_BB (cfg, not_an_array);
		}

		MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_BR, false_bb);
	} else {
		int klass_reg = alloc_preg (cfg);

		if (m_class_get_rank (klass)) {
			int rank_reg = alloc_preg (cfg);
			int eclass_reg = alloc_preg (cfg);

			if ((m_class_get_rank (klass) == 1) && (m_class_get_byval_arg (klass)->type == MONO_TYPE_SZARRAY)) {
				/* Check that the object is a vector too */
				int bounds_reg = alloc_preg (cfg);
				MONO_EMIT_NEW_LOAD_MEMBASE (cfg, bounds_reg, obj_reg, MONO_STRUCT_OFFSET (MonoArray, bounds));
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, bounds_reg, 0);
				MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_PBNE_UN, false_bb);
			}

			g_assert (!context_used);
			MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADU1_MEMBASE, rank_reg, vtable_reg, MONO_STRUCT_OFFSET (MonoVTable, rank));
			MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, rank_reg, m_class_get_rank (klass));
			MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_PBNE_UN, false_bb);
			MONO_EMIT_NEW_LOAD_MEMBASE (cfg, klass_reg, vtable_reg, MONO_STRUCT_OFFSET (MonoVTable, klass));
			MONO_EMIT_NEW_LOAD_MEMBASE (cfg, eclass_reg, klass_reg, GINTPTR_TO_TMREG (m_class_offsetof_cast_class ()));
			if (m_class_is_array_special_interface (m_class_get_cast_class (klass))) {
				MonoInst *move, *res_inst;

				res_inst = emit_isinst_with_cache (cfg, src, klass, context_used);
				EMIT_NEW_UNALU (cfg, move, OP_MOVE, res_reg, res_inst->dreg);
				MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_BR, end_bb);
			} else if (m_class_get_cast_class (klass) == mono_defaults.object_class) {
				int parent_reg, class_kind_reg;
				MonoBasicBlock *pointer_check_bb;

				NEW_BBLOCK (cfg, pointer_check_bb);

				parent_reg = alloc_preg (cfg);
				class_kind_reg = alloc_preg (cfg);
				MONO_EMIT_NEW_LOAD_MEMBASE (cfg, parent_reg, eclass_reg, GINTPTR_TO_TMREG (m_class_offsetof_parent ()));
				MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADU1_MEMBASE, class_kind_reg, eclass_reg, GINTPTR_TO_TMREG (m_class_offsetof_class_kind ()));

				// Check if the parent class of the element is not System.ValueType
				mini_emit_class_check_branch (cfg, parent_reg, m_class_get_parent (mono_defaults.enum_class), OP_PBNE_UN, pointer_check_bb);
				mini_emit_class_check_branch (cfg, eclass_reg, mono_defaults.enum_class, OP_PBEQ, is_null_bb);
				MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_BR, false_bb);

				MONO_START_BB (cfg, pointer_check_bb);
				// Check if the parent class of the element is non-null, else manually check the type
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, parent_reg, 0);
				MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_PBNE_UN, is_null_bb);
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, class_kind_reg, MONO_CLASS_POINTER);
				MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_PBEQ, false_bb);
				MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_BR, is_null_bb);
			} else if (m_class_get_cast_class (klass) == m_class_get_parent (mono_defaults.enum_class)) {
				mini_emit_class_check_branch (cfg, eclass_reg, m_class_get_parent (mono_defaults.enum_class), OP_PBEQ, is_null_bb);
				mini_emit_class_check_branch (cfg, eclass_reg, mono_defaults.enum_class, OP_PBEQ, is_null_bb);
				MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_BR, false_bb);
			} else if (m_class_get_cast_class (klass) == mono_defaults.enum_class) {
				mini_emit_class_check_branch (cfg, eclass_reg, mono_defaults.enum_class, OP_PBEQ, is_null_bb);
				MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_BR, false_bb);
			} else if (mono_class_is_interface (m_class_get_cast_class (klass))) {
				mini_emit_iface_class_cast (cfg, eclass_reg, m_class_get_cast_class (klass), false_bb, is_null_bb);
			} else {
				/* the is_null_bb target simply copies the input register to the output */
				mini_emit_isninst_cast (cfg, eclass_reg, m_class_get_cast_class (klass), false_bb, is_null_bb);
			}
		} else if (mono_class_is_nullable (klass)) {
			g_assert (!context_used);
			MONO_EMIT_NEW_LOAD_MEMBASE (cfg, klass_reg, vtable_reg, MONO_STRUCT_OFFSET (MonoVTable, klass));
			/* the is_null_bb target simply copies the input register to the output */
			mini_emit_isninst_cast (cfg, klass_reg, m_class_get_cast_class (klass), false_bb, is_null_bb);
		} else if (mono_class_is_sealed (klass)) {
#ifdef HOST_BROWSER
			MonoVTable *svt = cfg->compile_aot ? NULL : wj_sealed_vtable (cfg, klass);
#else
			MonoVTable *svt = NULL;
#endif
			g_assert (!context_used);
			if (cfg->compile_aot) {
				MonoInst *vtable_ins;
				EMIT_NEW_AOTCONST (cfg, vtable_ins, MONO_PATCH_INFO_VTABLE, klass);
				MONO_EMIT_NEW_BIALU (cfg, OP_COMPARE, -1, vtable_reg, vtable_ins->dreg);
			} else if (svt) {
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, vtable_reg, (gsize) svt);   /* B2 bit 2 */
			} else {
				MONO_EMIT_NEW_LOAD_MEMBASE (cfg, klass_reg, vtable_reg, MONO_STRUCT_OFFSET (MonoVTable, klass));
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, klass_reg, (gsize)klass);
			}
			MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_PBNE_UN, false_bb);
			MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_BR, is_null_bb);
		}
#ifdef HOST_BROWSER
		else if (wj_display_ok (cfg, klass, klass_inst)) {
			wj_emit_display_compare (cfg, vtable_reg, klass);   /* B2 bit 1; the is_null_bb target copies the input */
			MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_PBEQ, is_null_bb);
		}
#endif
		else {
			MONO_EMIT_NEW_LOAD_MEMBASE (cfg, klass_reg, vtable_reg, MONO_STRUCT_OFFSET (MonoVTable, klass));
			/* the is_null_bb target simply copies the input register to the output */
			mini_emit_isninst_cast_inst (cfg, klass_reg, klass, klass_inst, false_bb, is_null_bb);
		}
	}

	MONO_START_BB (cfg, false_bb);

	MONO_EMIT_NEW_PCONST (cfg, res_reg, NULL);
	MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_BR, end_bb);

	MONO_START_BB (cfg, is_null_bb);

	MONO_START_BB (cfg, end_bb);

	return ins;
}

#ifdef HOST_BROWSER
/* MONO_WASM_JIT_CAST_DIAG (mini-wasm.c): mark every InvalidCastException check the castclass decomposed into with a
 * tagged OP_DUMMY_USE of the object, placed ahead of the check's compare so that only the compare (no dreg) lies
 * between the mark and the OP_COND_EXC. The wasm emitter reads the mark from the OP_COND_EXC and raises with the
 * object and KLASS. The mark is a real use: the passes that run after this one keep the object's vreg live to it. */
extern int mono_wasm_jit_cast_diag;
extern const char mono_wasm_jit_cast_note_tag [];

static void
wj_cast_note_checks (MonoCompile *cfg, MonoBasicBlock *first_bb, MonoBasicBlock *last_bb, int obj_reg, MonoClass *klass)
{
	MonoBasicBlock *b;
	MonoInst *c, *note, *at;

	for (b = first_bb; b; b = b->next_bb) {
		for (c = b->code; c; c = c->next) {
			if (!MONO_IS_COND_EXC (c) || !c->inst_p1 || strcmp ((const char *) c->inst_p1, "InvalidCastException"))
				continue;
			at = c;
			if (c->prev && (c->prev->opcode == OP_COMPARE || c->prev->opcode == OP_COMPARE_IMM ||
			                c->prev->opcode == OP_ICOMPARE || c->prev->opcode == OP_ICOMPARE_IMM))
				at = c->prev;
			MONO_INST_NEW (cfg, note, OP_DUMMY_USE);
			note->sreg1 = obj_reg;
			note->klass = klass;
			note->inst_p1 = (gpointer) mono_wasm_jit_cast_note_tag;
			mono_bblock_insert_before_ins (b, at, note);
		}
		if (b == last_bb)
			break;
	}
}

/* MONO_WASM_JIT_CAST_UNFINISHED (mini-wasm.c): is KLASS -- or the element class an array of it is checked against, or
 * its generic definition -- a TypeBuilder's class that CreateType has not completed? Its parent, and so the depth the
 * inline check would bake, is not final until then; wastypebuilder is set under the loader lock after both are. */
extern int mono_wasm_jit_cast_unfinished;
void mono_wasm_jit_note_cast_unfinished (MonoMethod *method, int tier, MonoClass *klass, gboolean isinst, gboolean checked_at_run_time);

static gboolean
wj_cast_target_unfinished (MonoClass *klass)
{
	while (klass && m_class_get_rank (klass))
		klass = m_class_get_element_class (klass);
	if (klass && mono_class_is_ginst (klass))
		klass = mono_class_get_generic_class (klass)->container_class;
	return klass && image_is_dynamic (m_class_get_image (klass)) && !m_class_was_typebuilder (klass);
}

/* MONO_WASM_JIT_CAST_CENSUS (mini-wasm.c). A site is one castclass / isinst of one compile (tier 1 and 2, inlinees'
 * included); records are never freed, the counter word is baked into the emitted code. */
extern int mono_wasm_jit_cast_census;
enum { WJ_CC_OPEN, WJ_CC_STATIC, WJ_CC_DOMINATED, WJ_CC_RELOAD, WJ_CC_FINAL };
enum { WJ_CO_OTHER, WJ_CO_ARG, WJ_CO_CALL, WJ_CO_LOAD, WJ_CO_CAST, WJ_CO_MULTIDEF };
typedef struct WjCastSite {
	struct WjCastSite *next;
	char *cls, *meth, *target;
	gint32 il;                 /* IL offset in the compiled method, -1 inside an inlinee */
	guint8 tier, isinst, why, origin, erased, iface, sealed_, array;
	guint16 depth;
	volatile gint32 count;
} WjCastSite;
static WjCastSite *volatile wj_cc_sites;
static __thread MonoCompile *wj_cc_cfg;
static __thread MonoMethod *wj_cc_method;
static __thread GHashTable *wj_cc_erased;   /* MonoInst * pushed by an ldfld / ldsfld -> its MonoClassField, this compile */
static __thread GHashTable *wj_cc_site;     /* OP_CASTCLASS / OP_ISINST -> WjCastSite, this compile */
static __thread GHashTable *wj_cc_why;      /* OP_CASTCLASS / OP_ISINST -> its category + 1, this compile (census or L0) */
extern int mono_wasm_jit_cast_elim;
extern gint32 mono_wasm_jit_cast_elim_bad;

/* IKVM's erasure marker is a modopt (`object modopt(__<Unloadable>...)`); Java volatile is a modreq. Reads the
 * modifier list only: resolving the modifier's type in a compile section could load classes. */
static gboolean
wj_cc_type_erased (MonoType *t)
{
	MonoCustomModContainer *c;
	int i;
	if (!t || !t->has_cmods || ((MonoTypeWithModifiers *) t)->is_aggregate)
		return FALSE;
	c = mono_type_get_cmods (t);
	for (i = 0; c && i < c->count; ++i)
		if (!c->modifiers [i].required)
			return TRUE;
	return FALSE;
}

static void
wj_cc_reset (MonoCompile *cfg)
{
	if (wj_cc_cfg == cfg && wj_cc_method == cfg->method)
		return;
	wj_cc_cfg = cfg;
	wj_cc_method = cfg->method;
	if (wj_cc_erased) g_hash_table_remove_all (wj_cc_erased); else wj_cc_erased = g_hash_table_new (NULL, NULL);
	if (wj_cc_site) g_hash_table_remove_all (wj_cc_site); else wj_cc_site = g_hash_table_new (NULL, NULL);
	if (wj_cc_why) g_hash_table_remove_all (wj_cc_why); else wj_cc_why = g_hash_table_new (NULL, NULL);
}

/* method-to-ir.c, field_access_end: VAL is what an ldfld / ldsfld of a field of type FTYPE pushed. MonoInst.klass is
 * not initialised for every instruction (MONO_INST_NEW does not clear it), so the census takes a source's static class
 * only from here, a call's signature, a cast's own class, or the method signature. */
void mono_wasm_jit_cast_census_note_field (MonoCompile *cfg, MonoInst *val, MonoClassField *field);
void
mono_wasm_jit_cast_census_note_field (MonoCompile *cfg, MonoInst *val, MonoClassField *field)
{
	if (!val || !field || !field->type)
		return;
	wj_cc_reset (cfg);
	g_hash_table_insert (wj_cc_erased, val, field);
}

typedef struct { MonoInst *ins; MonoBasicBlock *bb; int pos; int root; } WjCcCast;

/* Does a value known to be HAVE pass a cast to TARGET? Parent chains only (supertypes of loaded classes, no loading
 * in a compile section); interface and array targets count on identity alone, so those categories are lower bounds. */
static gboolean
wj_cc_proves (MonoClass *target, MonoClass *have)
{
	if (!target || !have)
		return FALSE;
	if (target == have)
		return TRUE;
	if (mono_class_is_interface (target) || m_class_get_rank (target) || mono_class_is_interface (have) || m_class_get_rank (have))
		return FALSE;
	return mono_class_has_parent (have, target);
}

static int
wj_cc_root (int v, guint8 *ndef, MonoInst **def, int nv)
{
	int k;
	for (k = 0; k < 16 && v >= 0 && v < nv && ndef [v] == 1 && def [v] && def [v]->opcode == OP_MOVE && def [v]->sreg1 >= 0; ++k)
		v = def [v]->sreg1;
	return v;
}

static int
wj_cc_arg_index (MonoCompile *cfg, int v)
{
	int i, n = mono_method_signature_internal (cfg->method)->param_count + (mono_method_signature_internal (cfg->method)->hasthis ? 1 : 0);
	for (i = 0; cfg->args && i < n; ++i)
		if (cfg->args [i] && cfg->args [i]->dreg == v)
			return i;
	return -1;
}

static MonoType *
wj_cc_arg_type (MonoCompile *cfg, int i)
{
	MonoMethodSignature *sig = mono_method_signature_internal (cfg->method);
	if (sig->hasthis) {
		if (i == 0)
			return m_class_get_byval_arg (cfg->method->klass);
		i--;
	}
	return (i >= 0 && i < sig->param_count) ? sig->params [i] : NULL;
}

/* Classify every castclass / isinst of the compile into wj_cc_why; with CENSUS, also a site record (and its counter
 * address) per cast. */
static void
wj_cast_census_classify (MonoCompile *cfg, gboolean census)
{
	int nv = cfg->next_vreg, nb = cfg->num_bblocks, i, j, words, changed, iter;
	guint8 *ndef;
	MonoInst **def;
	GArray *casts;
	guint64 *dom = NULL, *tmp = NULL;
	MonoBasicBlock *bb;
	MonoInst *ins;
	MonoClass *object_class = mono_defaults.object_class;
	const char *mcls = m_class_get_name (cfg->method->klass), *mns = m_class_get_name_space (cfg->method->klass);

	wj_cc_reset (cfg);
	casts = g_array_new (FALSE, FALSE, sizeof (WjCcCast));
	ndef = g_new0 (guint8, nv);
	def = g_new0 (MonoInst *, nv);
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		int pos = 0;
		MONO_BB_FOR_EACH_INS (bb, ins) {
			if (ins->opcode == OP_CASTCLASS || ins->opcode == OP_ISINST) {
				WjCcCast c = { ins, bb, pos, -1 };
				g_array_append_val (casts, c);
			}
			if (ins->dreg >= 0 && ins->dreg < nv && ins->opcode != OP_NOP) {
				if (ndef [ins->dreg] < 255) ndef [ins->dreg]++;
				def [ins->dreg] = ins;
			}
			pos++;
		}
	}
	if (!casts->len)
		goto out;
	/* dominators over block_num (< num_bblocks before mono_bb_ordering), iterated to a fixed point; capped */
	words = (nb + 63) / 64;
	if (nb > 0 && nb <= 4096) {
		dom = g_new (guint64, (gsize) nb * words);
		tmp = g_new (guint64, words);
		for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
			guint64 *d = dom + (gsize) bb->block_num * words;
			if (bb == cfg->bb_entry) {
				memset (d, 0, words * 8);
				d [bb->block_num / 64] |= 1ull << (bb->block_num % 64);
			} else {
				memset (d, 0xff, words * 8);
			}
		}
		for (iter = 0, changed = 1; changed && iter < 64; ++iter) {
			changed = 0;
			for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
				guint64 *d = dom + (gsize) bb->block_num * words;
				if (bb == cfg->bb_entry)
					continue;
				if (bb->in_count == 0) {
					memset (tmp, 0, words * 8);
				} else {
					memset (tmp, 0xff, words * 8);
					for (i = 0; i < bb->in_count; ++i) {
						guint64 *p = dom + (gsize) bb->in_bb [i]->block_num * words;
						for (j = 0; j < words; ++j) tmp [j] &= p [j];
					}
				}
				tmp [bb->block_num / 64] |= 1ull << (bb->block_num % 64);
				if (memcmp (tmp, d, words * 8)) { memcpy (d, tmp, words * 8); changed = 1; }
			}
		}
		if (changed) { g_free (dom); dom = NULL; }   /* did not settle: no dominance claims */
	}
	for (i = 0; i < (int) casts->len; ++i) {
		WjCcCast *c = &g_array_index (casts, WjCcCast, i);
		c->root = wj_cc_root (c->ins->sreg1, ndef, def, nv);
	}
	for (i = 0; i < (int) casts->len; ++i) {
		WjCcCast *c = &g_array_index (casts, WjCcCast, i);
		MonoClass *klass = c->ins->klass;
		int v = c->root, argi = (v >= 0 && v < nv && ndef [v] == 0) ? wj_cc_arg_index (cfg, v) : -1;
		MonoInst *d = (v >= 0 && v < nv && ndef [v] == 1) ? def [v] : NULL;
		MonoInst *vvar = (v >= 0 && v < nv) ? get_vreg_to_inst (cfg, v) : NULL;
		/* a variable written through its address (ldloca / ldarga) or volatile (live into a handler) has writes ndef does
		 * not see: never single-definition for DOMINATED (its static type still holds for STATIC) */
		gboolean indirect = vvar && vvar != (MonoInst *) -1 && (vvar->flags & (MONO_INST_INDIRECT | MONO_INST_VOLATILE));
		gboolean single = (d != NULL || argi >= 0) && !indirect;
		MonoClass *src = NULL;
		WjCastSite site_tmp, *s = &site_tmp;
		MonoType *at = argi >= 0 ? wj_cc_arg_type (cfg, argi) : NULL;

		memset (&site_tmp, 0, sizeof (site_tmp));

		if (d && (d->opcode == OP_CASTCLASS || d->opcode == OP_ISINST)) {
			src = d->klass;
		} else if (d && MONO_IS_CALL (d)) {
			MonoMethodSignature *cs = ((MonoCallInst *) d)->signature;
			if (cs && cs->ret && MONO_TYPE_IS_REFERENCE (cs->ret))
				src = mono_class_from_mono_type_internal (cs->ret);
		} else if (d) {
			MonoClassField *fl = (MonoClassField *) g_hash_table_lookup (wj_cc_erased, d);
			if (fl && MONO_TYPE_IS_REFERENCE (fl->type))
				src = mono_class_from_mono_type_internal (fl->type);
		} else if (at && MONO_TYPE_IS_REFERENCE (at)) {
			src = mono_class_from_mono_type_internal (at);
		}
		s->why = WJ_CC_OPEN;
		if (klass && src && src != object_class && wj_cc_proves (klass, src))
			s->why = WJ_CC_STATIC;
		if (s->why == WJ_CC_OPEN && single && klass) {
			for (j = 0; j < (int) casts->len; ++j) {
				WjCcCast *p = &g_array_index (casts, WjCcCast, j);
				gboolean dominates;
				if (j == i || p->ins->opcode != OP_CASTCLASS || p->root != v || !p->ins->klass)
					continue;
				if (!wj_cc_proves (klass, p->ins->klass))
					continue;
				if (p->bb == c->bb)
					dominates = p->pos < c->pos;
				else
					dominates = dom && (dom [(gsize) c->bb->block_num * words + p->bb->block_num / 64] >> (p->bb->block_num % 64)) & 1;
				if (dominates) { s->why = WJ_CC_DOMINATED; break; }
			}
		}
		if (s->why == WJ_CC_OPEN && klass && d && MONO_IS_LOAD_MEMBASE (d) && d->sreg1 >= 0 && d->sreg1 < nv) {
			/* an earlier castclass in this block whose source is a load of the same base + offset, nothing that
			 * could store or call between the two loads */
			for (j = 0; j < (int) casts->len; ++j) {
				WjCcCast *p = &g_array_index (casts, WjCcCast, j);
				MonoInst *pd, *k;
				gboolean clean = TRUE, seen = FALSE;
				if (j == i || p->bb != c->bb || p->pos >= c->pos || p->ins->opcode != OP_CASTCLASS || !p->ins->klass)
					continue;
				pd = (p->root >= 0 && p->root < nv && ndef [p->root] == 1) ? def [p->root] : NULL;
				if (!pd || !MONO_IS_LOAD_MEMBASE (pd) || pd->sreg1 != d->sreg1 || pd->inst_offset != d->inst_offset)
					continue;
				if (!wj_cc_proves (klass, p->ins->klass))
					continue;
				MONO_BB_FOR_EACH_INS (c->bb, k) {
					if (k == pd) { seen = TRUE; continue; }
					if (k == d) break;
					if (seen && (MONO_IS_STORE_MEMBASE (k) || MONO_IS_CALL (k) || k->dreg == d->sreg1)) { clean = FALSE; break; }
				}
				if (seen && clean) { s->why = WJ_CC_RELOAD; break; }
			}
		}
		if (s->why == WJ_CC_OPEN && klass && d && MONO_IS_LOAD_MEMBASE (d)) {
			/* a read of an initonly (Java final) field whose same field on the same single-definition base (or the same
			 * static field) was read and castclass'd at a dominating position: the same value, checked once */
			MonoClassField *fl = (MonoClassField *) g_hash_table_lookup (wj_cc_erased, d);
			if (fl && (fl->type->attrs & FIELD_ATTRIBUTE_INIT_ONLY)) {
				gboolean stat = (fl->type->attrs & FIELD_ATTRIBUTE_STATIC) != 0;
				int b = stat ? -1 : wj_cc_root (d->sreg1, ndef, def, nv);
				gboolean bsingle = stat || (b >= 0 && b < nv && (ndef [b] == 1 || (ndef [b] == 0 && wj_cc_arg_index (cfg, b) >= 0)));
				for (j = 0; bsingle && j < (int) casts->len; ++j) {
					WjCcCast *p = &g_array_index (casts, WjCcCast, j);
					MonoInst *pd;
					gboolean dominates;
					if (j == i || p->ins->opcode != OP_CASTCLASS || !p->ins->klass || !wj_cc_proves (klass, p->ins->klass))
						continue;
					pd = (p->root >= 0 && p->root < nv && ndef [p->root] == 1) ? def [p->root] : NULL;
					if (!pd || !MONO_IS_LOAD_MEMBASE (pd) || g_hash_table_lookup (wj_cc_erased, pd) != fl)
						continue;
					if (!stat && wj_cc_root (pd->sreg1, ndef, def, nv) != b)
						continue;
					if (p->bb == c->bb)
						dominates = p->pos < c->pos;
					else
						dominates = dom && (dom [(gsize) c->bb->block_num * words + p->bb->block_num / 64] >> (p->bb->block_num % 64)) & 1;
					if (dominates) { s->why = WJ_CC_FINAL; break; }
				}
			}
		}
		if (argi >= 0) {
			s->origin = WJ_CO_ARG;
			s->erased = at && wj_cc_type_erased (at);
		} else if (!d) {
			s->origin = WJ_CO_MULTIDEF;
		} else if (MONO_IS_CALL (d)) {
			s->origin = WJ_CO_CALL;
			s->erased = ((MonoCallInst *) d)->signature && wj_cc_type_erased (((MonoCallInst *) d)->signature->ret);
		} else if (d->opcode == OP_CASTCLASS || d->opcode == OP_ISINST) {
			s->origin = WJ_CO_CAST;
		} else if (MONO_IS_LOAD_MEMBASE (d)) {
			s->origin = WJ_CO_LOAD;
			{ MonoClassField *fl = (MonoClassField *) g_hash_table_lookup (wj_cc_erased, d); s->erased = fl && wj_cc_type_erased (fl->type); }
		} else {
			s->origin = WJ_CO_OTHER;
		}
		if (!s->erased && c->ins->sreg1 != v && c->ins->sreg1 >= 0 && c->ins->sreg1 < nv && ndef [c->ins->sreg1] == 1)
			{ MonoClassField *fl = (MonoClassField *) g_hash_table_lookup (wj_cc_erased, def [c->ins->sreg1]); s->erased = fl && wj_cc_type_erased (fl->type); }
		g_hash_table_insert (wj_cc_why, c->ins, GINT_TO_POINTER (s->why + 1));
		if (!census)
			continue;
		s = g_new (WjCastSite, 1);
		*s = site_tmp;
		s->tier = (guint8) cfg->wasm_jit_tier;
		s->isinst = c->ins->opcode == OP_ISINST;
		s->iface = klass && mono_class_is_interface (klass);
		s->sealed_ = klass && mono_class_is_sealed (klass);
		s->array = klass && m_class_get_rank (klass) > 0;
		s->depth = klass ? m_class_get_idepth (klass) : 0;
		s->il = (c->ins->cil_code && cfg->header && c->ins->cil_code >= cfg->header->code &&
		         c->ins->cil_code < cfg->header->code + cfg->header->code_size) ? (gint32) (c->ins->cil_code - cfg->header->code) : -1;
		s->cls = g_strdup_printf ("%s%s%s", mns && *mns ? mns : "", mns && *mns ? "." : "", mcls);
		s->meth = g_strdup (cfg->method->name);
		if (klass) {
			const char *tn = m_class_get_name (klass), *tns = m_class_get_name_space (klass);
			s->target = g_strdup_printf ("%s%s%s", tns && *tns ? tns : "", tns && *tns ? "." : "", tn);
		} else {
			s->target = g_strdup ("?");
		}
		g_hash_table_insert (wj_cc_site, c->ins, s);
		{
			WjCastSite *h;
			do { h = wj_cc_sites; s->next = h; } while (mono_atomic_cas_ptr ((gpointer *) &wj_cc_sites, s, h) != h);
		}
	}
out:
	g_hash_table_remove_all (wj_cc_erased);   /* consumed: a later compile at a reused address must not see them */
	g_free (dom); g_free (tmp); g_free (ndef); g_free (def);
	g_array_free (casts, TRUE);
}

/* The site's counter, ahead of the check (cfg->cbb is the cast's first block). */
static void
wj_cast_census_count (MonoCompile *cfg, MonoInst *cast)
{
	WjCastSite *s;
	int a, v;
	if (wj_cc_cfg != cfg || wj_cc_method != cfg->method || !wj_cc_site || !(s = (WjCastSite *) g_hash_table_lookup (wj_cc_site, cast)))
		return;
	a = alloc_preg (cfg);
	v = alloc_ireg (cfg);
	MONO_EMIT_NEW_PCONST (cfg, a, (gpointer) &s->count);
	MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADI4_MEMBASE, v, a, 0);
	MONO_EMIT_NEW_BIALU_IMM (cfg, OP_IADD_IMM, v, v, 1);
	MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STOREI4_MEMBASE_REG, a, 0, v);
}

/* Every site that ran since the previous dump, then its count is cleared (the harness dumps at both edges of the
 * window, so the second dump's lines are the window). Not synchronized with running code: a census. */
void mono_wasm_jit_cast_census_dump (void);
void
mono_wasm_jit_cast_census_dump (void)
{
	static int seq;
	static const char *whyn [] = { "open", "static", "dominated", "reload", "final" };
	static const char *orgn [] = { "other", "arg", "call", "load", "cast", "multidef" };
	WjCastSite *s;
	gint64 tot = 0, by [5][2];
	int lines = 0;
	memset (by, 0, sizeof (by));
	++seq;
	for (s = wj_cc_sites; s; s = s->next) {
		gint32 n = s->count;
		if (n <= 0)
			continue;
		s->count = 0;
		tot += n; by [s->why][s->erased ? 1 : 0] += n;
		lines++;
		printf ("WASM_JIT_CAST\t%d\t%d\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%s:%s\n", seq, n, whyn [s->why], orgn [s->origin],
			s->erased, s->isinst, s->iface, s->sealed_, s->array, s->depth, s->tier, s->il, s->target, s->cls, s->meth);
	}
	printf ("[wasm-jit cast-census v3] dump %d: %d sites ran, %lld checks; static %lld (erased %lld), dominated %lld (%lld), reload %lld (%lld), final %lld (%lld), open %lld (%lld)\n",
		seq, lines, (long long) tot, (long long) (by [1][0] + by [1][1]), (long long) by [1][1], (long long) (by [2][0] + by [2][1]),
		(long long) by [2][1], (long long) (by [3][0] + by [3][1]), (long long) by [3][1], (long long) (by [4][0] + by [4][1]), (long long) by [4][1],
		(long long) (by [0][0] + by [0][1]), (long long) by [0][1]);
}
#endif

#ifdef HOST_BROWSER
/* L0 verify (MONO_WASM_JIT_CAST_ELIM bit 4): ahead of a check L0 would have removed, add (isinst (source) == null) -
 * (source == null) -- 1 exactly when a non-null value fails -- to mono_wasm_jit_cast_elim_bad. Branch-free; the check
 * itself follows unchanged. */
static void
wj_cast_elim_verify (MonoCompile *cfg, MonoClass *klass, MonoInst *source)
{
	MonoInst *r = handle_isinst (cfg, klass, source, 0);
	int a = alloc_preg (cfg), v = alloc_ireg (cfg), rz = alloc_ireg (cfg), sz = alloc_ireg (cfg), bad = alloc_ireg (cfg);
	MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ICOMPARE_IMM, -1, r->dreg, 0);
	MONO_EMIT_NEW_UNALU (cfg, OP_ICEQ, rz, -1);
	MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ICOMPARE_IMM, -1, source->dreg, 0);
	MONO_EMIT_NEW_UNALU (cfg, OP_ICEQ, sz, -1);
	MONO_EMIT_NEW_BIALU (cfg, OP_ISUB, bad, rz, sz);
	MONO_EMIT_NEW_PCONST (cfg, a, (gpointer) &mono_wasm_jit_cast_elim_bad);
	MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADI4_MEMBASE, v, a, 0);
	MONO_EMIT_NEW_BIALU (cfg, OP_IADD, v, v, bad);
	MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STOREI4_MEMBASE_REG, a, 0, v);
}
#endif

static void
mono_decompose_typecheck (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst *ins, gboolean verify_elim)
{
	MonoInst *ret, *move, *source;
	MonoClass *klass = ins->klass;
	int context_used = mini_class_check_context_used (cfg, klass);
	int is_isinst = ins->opcode == OP_ISINST;
	g_assert (is_isinst || ins->opcode == OP_CASTCLASS);
	source = get_vreg_to_inst (cfg, ins->sreg1);
	if (!source || source == (MonoInst *) -1)
		source = mono_compile_create_var_for_vreg (cfg, mono_get_object_type (), OP_LOCAL, ins->sreg1);
	g_assert (source && source != (MonoInst *) -1);

	MonoBasicBlock *first_bb;
	NEW_BBLOCK (cfg, first_bb);
	cfg->cbb = first_bb;
#ifdef HOST_BROWSER
	if (G_UNLIKELY (mono_wasm_jit_cast_census) && COMPILE_WASM (cfg))
		wj_cast_census_count (cfg, ins);
#endif

	gboolean with_cache = mini_class_has_reference_variant_generic_argument (cfg, klass, context_used);
#ifdef HOST_BROWSER
	if (!with_cache && !context_used && COMPILE_WASM (cfg) && wj_cast_target_unfinished (klass)) {
		with_cache = mono_wasm_jit_cast_unfinished != 0;
		mono_wasm_jit_note_cast_unfinished (cfg->method, cfg->wasm_jit_tier, klass, is_isinst, with_cache);
	}
#endif
#ifdef HOST_BROWSER
	if (verify_elim && !with_cache && !context_used)
		wj_cast_elim_verify (cfg, klass, source);
#endif
	if (with_cache) {
		if (is_isinst)
			ret = emit_isinst_with_cache (cfg, source, klass, context_used);
		else
			ret = emit_castclass_with_cache (cfg, source, klass, context_used);

	} else {
		if (is_isinst)
			ret = handle_isinst (cfg, klass, source, context_used);
		else
			ret = handle_castclass (cfg, klass, source, context_used);
#ifdef HOST_BROWSER
		if (!is_isinst && !context_used && COMPILE_WASM (cfg) && mono_wasm_jit_cast_diag)
			wj_cast_note_checks (cfg, first_bb, cfg->cbb, source->dreg, klass);
#endif
	}
	EMIT_NEW_UNALU (cfg, move, OP_MOVE, ins->dreg, ret->dreg);

	g_assert (cfg->cbb->code || first_bb->code);
	MonoInst *prev = ins->prev;
	mono_replace_ins (cfg, bb, ins, &prev, first_bb, cfg->cbb);
}

void
mono_decompose_typechecks (MonoCompile *cfg)
{
	gboolean found_typetest = FALSE;
	int elim = 0;
	(void) elim;
#ifdef HOST_BROWSER
	if (G_UNLIKELY (mono_wasm_jit_cast_census || mono_wasm_jit_cast_elim) && COMPILE_WASM (cfg)) {
		wj_cast_census_classify (cfg, mono_wasm_jit_cast_census != 0);
		elim = mono_wasm_jit_cast_elim;
	}
#endif
	for (MonoBasicBlock *bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		MonoInst *ins;
		MONO_BB_FOR_EACH_INS (bb, ins) {
			switch (ins->opcode) {
			case OP_ISINST:
			case OP_CASTCLASS: {
				gboolean verify_elim = FALSE;
				found_typetest = TRUE;
#ifdef HOST_BROWSER
				/* L0: a check the classification proves always passes is a move (bits 1 / 2), or is verified (bit 4) */
				if (elim && wj_cc_why && wj_cc_cfg == cfg) {
					int why = GPOINTER_TO_INT (g_hash_table_lookup (wj_cc_why, ins)) - 1;
					if ((why == WJ_CC_STATIC && (elim & 1)) || (why == WJ_CC_DOMINATED && (elim & 2))) {
						mono_wasm_jit_counters [why == WJ_CC_STATIC ? WJC_CAST_ELIM_STATIC : WJC_CAST_ELIM_DOMINATED]++;
						if (elim & 4) {
							verify_elim = TRUE;
						} else {
							ins->opcode = OP_MOVE;   /* dreg <- sreg1: the value, unchanged; null stays null */
							break;
						}
					}
				}
#endif
				mono_decompose_typecheck (cfg, bb, ins, verify_elim);
				break;
			}
			}
		}
	}
#ifdef HOST_BROWSER
	if (G_UNLIKELY (mono_wasm_jit_cast_census || mono_wasm_jit_cast_elim) && COMPILE_WASM (cfg) && wj_cc_cfg == cfg) {
		if (wj_cc_site)
			g_hash_table_remove_all (wj_cc_site);
		if (wj_cc_why)
			g_hash_table_remove_all (wj_cc_why);
		wj_cc_cfg = NULL; wj_cc_method = NULL;
	}
#endif
	if ((cfg->verbose_level > 2) && found_typetest)
		mono_print_code (cfg, "AFTER DECOMPOSE TYPE_CHECKS");

}


//API used by method-to-ir.c
void
mini_emit_class_check (MonoCompile *cfg, int klass_reg, MonoClass *klass)
{
	mini_emit_class_check_inst (cfg, klass_reg, klass, NULL);
}

#else

MONO_EMPTY_SOURCE_FILE (type_checking);
#endif
