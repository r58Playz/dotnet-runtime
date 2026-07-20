// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;

public static class WasmJitEhAotBoundaryTests
{
    public sealed class ProbeException : Exception
    {
        public ProbeException(int marker) : base($"probe-{marker}") => Marker = marker;

        public int Marker { get; }
    }

    public sealed class StateBox
    {
        public int Value;
    }

    private delegate int CatchDelegate(ProbeException exception);
    private delegate void ThrowDelegate(ProbeException exception);
    private delegate void FinallyDelegate(ProbeException exception, StateBox state);

    public static int Main()
    {
        try
        {
            CatchDelegate jitCatchAotThrow = CreateJitCatchAotThrow();
            ThrowDelegate jitThrow = CreateJitThrow();
            ThrowDelegate jitRethrowAotThrow = CreateJitRethrowAotThrow();
            FinallyDelegate jitFinally = CreateJitFinally();

            Equal(501, jitCatchAotThrow(new ProbeException(501)), "AOT throw to JIT catch");
            Equal(502, AotCatchJitThrow(jitThrow, new ProbeException(502)), "JIT throw to AOT catch");

            ProbeException rethrown = new ProbeException(503);
            try
            {
                jitRethrowAotThrow(rethrown);
                Fail("JIT rethrow to AOT returned");
            }
            catch (ProbeException ex)
            {
                Same(rethrown, ex, "AOT throw / JIT rethrow / AOT catch identity");
            }

            StateBox state = new StateBox();
            Equal(504, AotCatchJitFinally(jitFinally, new ProbeException(504), state),
                "JIT finally to AOT catch");
            Equal(1, state.Value, "JIT exceptional finally ran once");

            state = new StateBox();
            ProbeException throughAotFinally = new ProbeException(505);
            try
            {
                AotFinallyJitThrow(jitThrow, throughAotFinally, state);
                Fail("AOT finally around JIT throw returned");
            }
            catch (ProbeException ex)
            {
                Same(throughAotFinally, ex, "JIT throw / AOT finally identity");
                Equal(1, state.Value, "AOT exceptional finally ran once");
            }

            state = new StateBox();
            Equal(506, AotFinallyJitCatch(jitCatchAotThrow, new ProbeException(506), state),
                "AOT to JIT to AOT round trip");
            Equal(1, state.Value, "AOT normal finally after JIT catch ran once");

            Console.WriteLine("WASM_JIT_AOT_EH_BOUNDARY_PASS");
            return 42;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"WASM_JIT_AOT_EH_BOUNDARY_FAIL: {ex}");
            return 1;
        }
    }

    // These methods are part of the statically linked application assembly in the AOT test.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void AotThrow(ProbeException exception) => throw exception;

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int AotCatchJitThrow(ThrowDelegate thrower, ProbeException exception)
    {
        try
        {
            thrower(exception);
            return -1;
        }
        catch (ProbeException ex)
        {
            return ex.Marker;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int AotCatchJitFinally(FinallyDelegate thrower, ProbeException exception, StateBox state)
    {
        try
        {
            thrower(exception, state);
            return -1;
        }
        catch (ProbeException ex)
        {
            return ex.Marker;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void AotFinallyJitThrow(ThrowDelegate thrower, ProbeException exception, StateBox state)
    {
        try
        {
            thrower(exception);
        }
        finally
        {
            state.Value++;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int AotFinallyJitCatch(CatchDelegate catcher, ProbeException exception, StateBox state)
    {
        try
        {
            return catcher(exception);
        }
        finally
        {
            state.Value++;
        }
    }

    private static CatchDelegate CreateJitCatchAotThrow()
    {
        DynamicMethod method = NewDynamicMethod(
            "WasmJitEhAotThrowToJitCatch",
            typeof(int),
            new[] { typeof(ProbeException) });
        ILGenerator il = method.GetILGenerator();
        LocalBuilder result = il.DeclareLocal(typeof(int));
        Label end = il.BeginExceptionBlock();
        il.Emit(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Call, AotThrowMethod);
        il.Emit(OpCodes.Ldc_I4_M1);
        il.Emit(OpCodes.Stloc, result);
        il.Emit(OpCodes.Leave, end);
        il.BeginCatchBlock(typeof(ProbeException));
        il.Emit(OpCodes.Callvirt, MarkerGetter);
        il.Emit(OpCodes.Stloc, result);
        il.Emit(OpCodes.Leave, end);
        il.EndExceptionBlock();
        il.Emit(OpCodes.Ldloc, result);
        il.Emit(OpCodes.Ret);
        return method.CreateDelegate<CatchDelegate>();
    }

    private static ThrowDelegate CreateJitThrow()
    {
        DynamicMethod method = NewDynamicMethod(
            "WasmJitEhJitThrowToAotCatch",
            typeof(void),
            new[] { typeof(ProbeException) });
        ILGenerator il = method.GetILGenerator();
        il.Emit(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Throw);
        return method.CreateDelegate<ThrowDelegate>();
    }

    private static ThrowDelegate CreateJitRethrowAotThrow()
    {
        DynamicMethod method = NewDynamicMethod(
            "WasmJitEhAotThrowToJitRethrow",
            typeof(void),
            new[] { typeof(ProbeException) });
        ILGenerator il = method.GetILGenerator();
        Label end = il.BeginExceptionBlock();
        il.Emit(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Call, AotThrowMethod);
        il.Emit(OpCodes.Leave, end);
        il.BeginCatchBlock(typeof(ProbeException));
        il.Emit(OpCodes.Pop);
        il.Emit(OpCodes.Rethrow);
        il.EndExceptionBlock();
        il.Emit(OpCodes.Ret);
        return method.CreateDelegate<ThrowDelegate>();
    }

    private static FinallyDelegate CreateJitFinally()
    {
        DynamicMethod method = NewDynamicMethod(
            "WasmJitEhJitFinallyToAotCatch",
            typeof(void),
            new[] { typeof(ProbeException), typeof(StateBox) });
        ILGenerator il = method.GetILGenerator();
        il.BeginExceptionBlock();
        il.Emit(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Throw);
        il.BeginFinallyBlock();
        il.Emit(OpCodes.Ldarg_1);
        il.Emit(OpCodes.Dup);
        il.Emit(OpCodes.Ldfld, StateValueField);
        il.Emit(OpCodes.Ldc_I4_1);
        il.Emit(OpCodes.Add);
        il.Emit(OpCodes.Stfld, StateValueField);
        il.EndExceptionBlock();
        il.Emit(OpCodes.Ret);
        return method.CreateDelegate<FinallyDelegate>();
    }

    private static DynamicMethod NewDynamicMethod(string name, Type returnType, Type[] parameterTypes) =>
        new DynamicMethod(name, returnType, parameterTypes, typeof(WasmJitEhAotBoundaryTests), skipVisibility: true);

    // A direct delegate keeps the AOT target visible to the trimmer while still
    // giving Reflection.Emit the MethodInfo needed by the dynamic wrapper.
    private static readonly MethodInfo AotThrowMethod = ((Action<ProbeException>)AotThrow).Method;

    private static readonly MethodInfo MarkerGetter =
        typeof(ProbeException).GetProperty(nameof(ProbeException.Marker))!.GetMethod!;

    private static readonly FieldInfo StateValueField =
        typeof(StateBox).GetField(nameof(StateBox.Value))!;

    private static void Equal(int expected, int actual, string path)
    {
        if (expected != actual)
            throw new Exception($"{path}: expected {expected}, got {actual}");
    }

    private static void Same(object expected, object actual, string path)
    {
        if (!ReferenceEquals(expected, actual))
            throw new Exception($"{path}: exception identity changed");
    }

    private static void Fail(string path) => throw new Exception(path);
}
