// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Wasm-JIT by-address value-type ABI tests (WS-B B3): multi-field structs mixing managed refs and
// scalars (the IKVM MHA`8 shape) passed as by-addr args and returned via the hidden vret pointer,
// across every boundary: interp->JIT (e-thunk), JIT->JIT (f-slot), JIT->interp (residual), and
// delegate dispatch (MINT_CALL_DELEGATE redirect).

using System;
using System.Runtime.CompilerServices;

public static class WasmJitVtypeTests
{
    // The IKVM.Runtime.MHA`8 shape: 8 generic fields, fully instantiated mixing object refs and ints.
    public struct MHA8<T1, T2, T3, T4, T5, T6, T7, T8>
    {
        public T1 t1;
        public T2 t2;
        public T3 t3;
        public T4 t4;
        public T5 t5;
        public T6 t6;
        public T7 t7;
        public T8 t8;

        public MHA8(T1 t1, T2 t2, T3 t3, T4 t4, T5 t5, T6 t6, T7 t7, T8 t8)
        {
            this.t1 = t1; this.t2 = t2; this.t3 = t3; this.t4 = t4;
            this.t5 = t5; this.t6 = t6; this.t7 = t7; this.t8 = t8;
        }
    }

    public sealed class Tag
    {
        public Tag(int id) => Id = id;
        public int Id { get; }
    }

    public struct ScalarInt
    {
        public int Value;
        public ScalarInt(int value) => Value = value;
    }

    public struct ScalarRef
    {
        public object Value;
        public ScalarRef(object value) => Value = value;
    }

    public struct ScalarLong
    {
        public long Value;
        public ScalarLong(long value) => Value = value;
    }

    public struct ScalarDouble
    {
        public double Value;
        public ScalarDouble(double value) => Value = value;
    }

    public abstract class ScalarVirtual
    {
        public abstract int VtVirtualTake(ScalarInt value);
    }

    public sealed class ScalarVirtualImpl : ScalarVirtual
    {
        [MethodImpl(MethodImplOptions.NoInlining)]
        public override int VtVirtualTake(ScalarInt value) => value.Value + 1;
    }

    public abstract class MhaVirtual
    {
        public abstract int VtVirtualTakeMha(MHA8<object, int, int, object, object, int, int, int> value);
    }

    public sealed class MhaVirtualImpl : MhaVirtual
    {
        [MethodImpl(MethodImplOptions.NoInlining)]
        public override int VtVirtualTakeMha(MHA8<object, int, int, object, object, int, int, int> value) =>
            value.t2 + value.t3 + value.t6;
    }

    public delegate int MhaTaker(object o, int k, MHA8<object, int, int, object, object, int, int, int> m);
    public delegate int IntTaker(int value);

    public sealed class IntTarget
    {
        public IntTarget(int bias) => Bias = bias;
        public int Bias { get; }
        public int Calls { get; private set; }

        [MethodImpl(MethodImplOptions.NoInlining)]
        public int VtDelegateAdd(int value)
        {
            Calls++;
            return Bias + value;
        }
    }

    public static int Main()
    {
        try
        {
            Tag tag = new Tag(7);
            var m = new MHA8<object, int, int, object, object, int, int, int>(tag, 10, 20, tag, tag, 30, 40, 50);

            // interp caller -> JITted callee with a by-addr arg (e-thunk passes args_ptr+off)
            Equal(7 + 10 + 20 + 30, VtTakeMHA(tag, 0, m), "interp to JIT by-addr arg");

            // byval semantics: callee mutation must not leak into the caller's copy
            Equal(4242, VtMutateMHA(m), "callee mutation result");
            Equal(10, m.t2, "caller copy isolated from callee mutation");
            Same(tag, m.t1, "caller ref field isolated from callee mutation");

            // JITted caller -> JITted callee, loop re-copy + mutation isolation inside JIT code
            Equal(3 * (7 + 5 + 20 + 30) + 4242 + 5, VtCallMHA(tag, 5), "JIT to JIT by-addr arg loop");

            // hidden vret: interp caller -> JITted callee, then JITted caller -> JITted vret callee
            var made = VtMakeMHA(tag, 11);
            Equal(11, made.t2, "interp to JIT vret scalar field");
            Same(tag, made.t1, "interp to JIT vret ref field");
            Equal(11 + 12, VtUseMakeMHA(tag, 11), "JIT to JIT vret");

            // JITted caller -> interp-resident callee (perm-bailed): residual with a by-addr arg
            Equal(7 + 3 + 21, VtResidualTake(tag, 3), "JIT to interp residual by-addr arg");
            // ... and a residual hidden vret
            Equal(9 + 1, VtResidualMake(tag, 9), "JIT to interp residual vret");

            // Delegate.Invoke from a JITted caller: open-static/by-address takes the direct-target
            // residual marshal, while closed-instance/scalar can enter the target e-thunk directly.
            MhaTaker taker = VtTakeMHA;
            Equal(7 + 10 + 20 + 30, VtInvokeMhaDelegate(taker, tag, m), "JIT delegate to JIT by-addr arg");
            var intTarget = new IntTarget(9);
            IntTaker closed = intTarget.VtDelegateAdd;
            Equal(13, VtInvokeIntDelegate(closed, 4), "JIT closed delegate to JIT scalar target");

            // Multicast must retain the generated Invoke wrapper (including every-target execution and
            // last-result semantics), never take the single-cast direct-target recipe.
            IntTaker multicast = closed;
            multicast += VtDelegateDouble;
            Equal(8, VtInvokeIntDelegate(multicast, 4), "JIT multicast delegate wrapper");
            Equal(2, intTarget.Calls, "multicast invoked first target");

            // Bound-static (first-arg-bound) delegates: a REFERENCE bound arg is eligible for the
            // direct-target recipe; a BOXED VALUE-TYPE bound arg must stay on the generated wrapper,
            // whose UNBOX_ANY the recipe path deliberately does not replicate (it would pass the
            // box's pointer bits as the value).
            IntTaker boundRef = (IntTaker)Delegate.CreateDelegate(typeof(IntTaker), intTarget,
                typeof(WasmJitVtypeTests).GetMethod(nameof(VtBoundRefBias)));
            Equal(9 + 6, VtInvokeIntDelegate(boundRef, 6), "JIT bound-static ref-arg delegate");
            IntTaker boundInt = (IntTaker)Delegate.CreateDelegate(typeof(IntTaker), 100,
                typeof(WasmJitVtypeTests).GetMethod(nameof(VtBoundIntBias)));
            Equal(100 + 5, VtInvokeIntDelegate(boundInt, 5), "JIT bound-static boxed-int delegate (wrapper unbox)");

            // ArgVtypeAsScalar callee reconstruction, both ref-free and a managed-reference field.
            Equal(1234, VtTakeScalar(new ScalarInt(1234)), "scalar-vtype int callee");
            Equal(7, VtTakeScalarRef(new ScalarRef(tag)), "scalar-vtype ref callee");
            Equal(2468, VtUseScalarReturn(2468), "scalar-vtype return");
            Equal(7, VtUseScalarRefReturn(tag), "scalar-vtype ref return");
            // wide-etype scalar vtypes: i64/f64 single-field structs exercise the e-thunk I64/F64
            // arg loads and the addr-frame scratch groups (wj_store_scalar_vtype_result addr_tmp[g])
            // that the int/ref probes never touch
            Equal(123456789012345L, VtTakeScalarLong(new ScalarLong(123456789012345L)), "scalar-vtype long callee");
            Equal(987654321098765L, VtUseScalarLongReturn(987654321098765L), "scalar-vtype long return");
            Equal(2.75, VtTakeScalarDouble(new ScalarDouble(2.25)), "scalar-vtype double callee");
            Equal(6.75, VtUseScalarDoubleReturn(6.75), "scalar-vtype double return");
            Equal(1357, VtResidualScalarReturn(1357), "residual scalar-vtype return");
            Equal(9754, VtVirtualScalarArg(new ScalarVirtualImpl(), 9753), "virtual scalar-vtype arg");
            Equal(60, VtVirtualByaddrArg(new MhaVirtualImpl(), m), "virtual by-address vtype arg");

            // Arguments following an inline vtype must use the shifted interpreter offsets in the e-thunk.
            Equal(7 + 10 + 77 + 3 + 1, VtMixedOffsets(tag, m, 77, 3.5, tag), "mixed args after by-addr vtype");

            // GC: refs embedded in a by-addr struct stay alive/pinned across collections
            Equal(123 + 456, VtAcrossGc(new Tag(123), new Tag(456)), "by-addr refs across GC");
            Equal(321 + 654, VtGcInside(new MHA8<object, int, int, object, object, int, int, int>(
                new Tag(321), 1, 2, new Tag(654), null, 3, 4, 5)), "incoming by-addr refs during GC");

            // nested MHA (t8 = inner MHA): bigger flat struct through the same path
            Equal(5 + 10 + 10, VtCallNested(tag, 5), "nested MHA by-addr arg");

            // ref-bearing struct LOCAL, address taken (full-size addr-frame slot)
            Equal(2 + 33, VtRefLocal(tag, 33), "ref-bearing vtype local");

            Console.WriteLine("WASM_JIT_VTYPE_TEST_PASS");
            return 42;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"WASM_JIT_VTYPE_TEST_FAIL {ex}");
            return 1;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtTakeMHA(object o, int k, MHA8<object, int, int, object, object, int, int, int> m)
    {
        int refs = (m.t1 is Tag t) ? t.Id : 0;
        return refs + k + m.t2 + m.t3 + m.t6;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtInvokeMhaDelegate(MhaTaker taker, object o,
        MHA8<object, int, int, object, object, int, int, int> m) => taker(o, 0, m);

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtInvokeIntDelegate(IntTaker taker, int value) => taker(value);

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtDelegateDouble(int value) => value * 2;

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtMutateMHA(MHA8<object, int, int, object, object, int, int, int> m)
    {
        m.t2 = 4242;
        m.t1 = null;
        return m.t2;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtTakeScalar(ScalarInt value) => value.Value;

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtTakeScalarRef(ScalarRef value) => ((Tag)value.Value).Id;

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtVirtualScalarArg(ScalarVirtual target, int value) => target.VtVirtualTake(new ScalarInt(value));

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtVirtualByaddrArg(MhaVirtual target,
        MHA8<object, int, int, object, object, int, int, int> value) => target.VtVirtualTakeMha(value);

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtMixedOffsets(object first,
        MHA8<object, int, int, object, object, int, int, int> value,
        long afterLong, double afterDouble, object afterObject)
    {
        return ((Tag)first).Id + value.t2 + (int)afterLong + (afterDouble == 3.5 ? 3 : -3000) + (afterObject != null ? 1 : 0);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static ScalarInt VtMakeScalar(int value) => new ScalarInt(value);

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static ScalarRef VtMakeScalarRef(object value) => new ScalarRef(value);

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtUseScalarReturn(int value)
    {
        ScalarInt result = VtMakeScalar(value);
        return result.Value;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtUseScalarRefReturn(Tag value)
    {
        ScalarRef result = VtMakeScalarRef(value);
        return ((Tag)result.Value).Id;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static long VtTakeScalarLong(ScalarLong value) => value.Value;

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static ScalarLong VtMakeScalarLong(long value) => new ScalarLong(value);

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static long VtUseScalarLongReturn(long value)
    {
        ScalarLong result = VtMakeScalarLong(value);
        return result.Value;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static double VtTakeScalarDouble(ScalarDouble value) => value.Value + 0.5;

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static ScalarDouble VtMakeScalarDouble(double value) => new ScalarDouble(value);

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static double VtUseScalarDoubleReturn(double value)
    {
        ScalarDouble result = VtMakeScalarDouble(value);
        return result.Value;
    }

    // Bound-static (first-arg-bound) delegate targets: the ref-arg one is eligible for the wasm-JIT
    // direct-target recipe; the int-arg one must fall back to the generated wrapper (unbox).
    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtBoundRefBias(IntTarget target, int value) => target.Bias + value;

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtBoundIntBias(int bias, int value) => bias + value;

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static ScalarInt VtFilteredScalarReturn(int value)
    {
        try
        {
            return new ScalarInt(value);
        }
        catch (Exception ex) when (ex.Message.Length > value)
        {
            return default;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtResidualScalarReturn(int value)
    {
        ScalarInt result = VtFilteredScalarReturn(value);
        return result.Value;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtCallMHA(object o, int k)
    {
        var m = new MHA8<object, int, int, object, object, int, int, int>(o, k, 20, o, o, 30, 40, 50);
        int r = 0;
        for (int i = 0; i < 3; i++)
            r += VtTakeMHA(o, 0, m);   // per-iteration re-copy: a mutating callee ran between iterations
        r += VtMutateMHA(m);
        r += m.t2;                     // must still be k, not 4242
        return r - k + k;              // keep k live
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static MHA8<object, int, int, object, object, int, int, int> VtMakeMHA(object o, int k)
    {
        return new MHA8<object, int, int, object, object, int, int, int>(o, k, k + 1, o, o, k + 2, k + 3, k + 4);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtUseMakeMHA(object o, int k)
    {
        var m = VtMakeMHA(o, k);
        return m.t2 + m.t3;
    }

    // Perm-unjittable (EH filter) MHA consumer: a JITted caller reaches it only via the interp
    // residual, exercising the by-addr scratch-slot deref (mono_wasm_jit_arg_is_byaddr).
    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtFilteredTake(int k, MHA8<object, int, int, object, object, int, int, int> m)
    {
        try
        {
            int refs = (m.t1 is Tag t) ? t.Id : 0;
            return refs + k + m.t3 - m.t2 + m.t2;
        }
        catch (Exception ex) when (ex.Message.Length > k)
        {
            return -1;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtResidualTake(object o, int k)
    {
        var m = new MHA8<object, int, int, object, object, int, int, int>(o, 1, 21, o, o, 2, 3, 4);
        return VtFilteredTake(k, m);
    }

    // Perm-unjittable (EH filter) vret producer for the residual hidden-vret path.
    [MethodImpl(MethodImplOptions.NoInlining)]
    public static MHA8<object, int, int, object, object, int, int, int> VtFilteredMake(object o, int k)
    {
        try
        {
            return new MHA8<object, int, int, object, object, int, int, int>(o, k, k + 1, o, o, 0, 0, 0);
        }
        catch (Exception ex) when (ex.Message.Length > k)
        {
            return default;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtResidualMake(object o, int k)
    {
        var m = VtFilteredMake(o, k);
        return m.t2 + (m.t1 is Tag ? 1 : 0);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtAcrossGc(Tag a, Tag b)
    {
        var m = new MHA8<object, int, int, object, object, int, int, int>(a, 1, 2, b, a, 3, 4, 5);
        a = null;
        b = null;
        for (int i = 0; i < 64; i++)
        {
            byte[] garbage = new byte[8192];
            garbage[0] = (byte)i;
            if (i == 32)
                GC.Collect();
        }
        GC.Collect();
        return VtSumTags(0, m);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtSumTags(int k, MHA8<object, int, int, object, object, int, int, int> m)
    {
        return k + ((Tag)m.t1).Id + ((Tag)m.t4).Id;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtGcInside(MHA8<object, int, int, object, object, int, int, int> m)
    {
        for (int i = 0; i < 64; i++)
        {
            byte[] garbage = new byte[8192];
            garbage[0] = (byte)i;
        }
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
        return ((Tag)m.t1).Id + ((Tag)m.t4).Id;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtTakeNested(int k, MHA8<object, int, int, object, int, int, object, MHA8<object, int, int, object, object, int, int, int>> m)
    {
        return k + m.t2 + m.t8.t2;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtCallNested(object o, int k)
    {
        var inner = new MHA8<object, int, int, object, object, int, int, int>(o, 10, 3, o, o, 6, 7, 8);
        var outer = new MHA8<object, int, int, object, int, int, object, MHA8<object, int, int, object, object, int, int, int>>(o, 10, 3, o, 5, 6, o, inner);
        return VtTakeNested(k, outer);
    }

    public struct RefPair
    {
        public object a;
        public int b;
        public object c;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtRefLocal(object o, int k)
    {
        RefPair p;
        p.a = o;
        p.b = k;
        p.c = o;
        return VtConsumeRef(ref p) + (p.a != null ? 0 : -1000);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    public static int VtConsumeRef(ref RefPair p)
    {
        return (p.a != null ? 1 : 0) + (p.c != null ? 1 : 0) + p.b;
    }

    private static void Equal(int expected, int actual, string path)
    {
        if (expected != actual)
            throw new Exception($"{path}: expected {expected}, got {actual}");
    }

    private static void Equal(long expected, long actual, string path)
    {
        if (expected != actual)
            throw new Exception($"{path}: expected {expected}, got {actual}");
    }

    private static void Equal(double expected, double actual, string path)
    {
        if (expected != actual)
            throw new Exception($"{path}: expected {expected}, got {actual}");
    }

    private static void Same(object expected, object actual, string path)
    {
        if (!ReferenceEquals(expected, actual))
            throw new Exception($"{path}: reference identity changed");
    }
}
