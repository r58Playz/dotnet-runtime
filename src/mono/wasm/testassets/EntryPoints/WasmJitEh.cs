// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;

public static class WasmJitEhTests
{
    public sealed class ProbeException : Exception
    {
        public ProbeException(int marker) : base($"probe-{marker}") => Marker = marker;

        public int Marker { get; }
    }

    public sealed class StateBox
    {
        public int Value;
        public int Other;
    }

    public static int Main()
    {
        try
        {
            Equal(101, WasmJitEhCatch(new ProbeException(101)), "local catch");
            Equal(102, WasmJitEhConditionalThrow(1), "conditional corlib throw");
            Equal(103, WasmJitEhTypedCatch(new ProbeException(103)), "typed catch ordering");
            Equal(104, WasmJitEhCatchJitCallee(new ProbeException(104)), "JIT to JIT catch");
            Equal(105, WasmJitEhCatchInterpCallee(new ProbeException(105)), "interpreter residual to JIT catch");
            Equal(106, WasmJitEhCatchAcrossGc(new ProbeException(106)), "caught exception GC root");

            ProbeException rethrown = new ProbeException(107);
            try
            {
                WasmJitEhRethrow(rethrown);
                Fail("rethrow returned");
            }
            catch (ProbeException ex)
            {
                Same(rethrown, ex, "rethrow identity");
            }

            ProbeException catchOriginal = new ProbeException(117);
            ProbeException catchReplacement = new ProbeException(118);
            try
            {
                WasmJitEhCatchThrowsReplacement(catchOriginal, catchReplacement);
                Fail("catch replacement returned");
            }
            catch (ProbeException ex)
            {
                Same(catchReplacement, ex, "catch replacement identity");
            }

            StateBox state = new StateBox();
            Equal(7, WasmJitEhFinallyNormal(state), "normal finally return");
            Equal(3, state.Value, "normal finally side effect");

            state = new StateBox();
            ProbeException finallyThrown = new ProbeException(108);
            try
            {
                WasmJitEhFinallyThrow(finallyThrown, state);
                Fail("exceptional finally returned");
            }
            catch (ProbeException ex)
            {
                Same(finallyThrown, ex, "exceptional finally identity");
                Equal(4, state.Value, "exceptional finally side effect");
            }

            state = new StateBox();
            ProbeException finallyOriginal = new ProbeException(119);
            ProbeException finallyReplacement = new ProbeException(120);
            try
            {
                WasmJitEhFinallyReplacesException(finallyOriginal, finallyReplacement, state);
                Fail("finally replacement returned");
            }
            catch (ProbeException ex)
            {
                Same(finallyReplacement, ex, "finally replacement identity");
                Equal(1, state.Value, "finally replacement side effect");
            }

            state = new StateBox();
            Equal(9, WasmJitEhCatchInsideNormalFinally(state), "catch inside normal finally return");
            Equal(5, state.Value, "catch inside normal finally continuation");

            state = new StateBox();
            ProbeException outer = new ProbeException(109);
            try
            {
                WasmJitEhCatchInsideExceptionalFinally(outer, state);
                Fail("exceptional nested catch returned");
            }
            catch (ProbeException ex)
            {
                Same(outer, ex, "finally preserved outer exception");
                Equal(6, state.Value, "nested catch in exceptional finally");
            }

            state = new StateBox();
            Equal(111, WasmJitEhOuterCatchInnerFinally(new ProbeException(110), state), "outer JIT catch / inner JIT finally");
            Equal(1, state.Value, "inner JIT finally ran once");

            ProbeException mismatched = new ProbeException(111);
            try
            {
                WasmJitEhMismatchedCatch(mismatched);
                Fail("mismatched catch returned");
            }
            catch (ProbeException ex)
            {
                Same(mismatched, ex, "unmatched JIT catch propagated");
            }

            Equal(112, WasmJitEhNullThrow(), "throw null");

            state = new StateBox();
            ProbeException faultThrown = new ProbeException(113);
            try
            {
                CreateFaultMethod()(state, faultThrown);
                Fail("fault method returned");
            }
            catch (ProbeException ex)
            {
                Same(faultThrown, ex, "fault exception identity");
                Equal(1, state.Value, "fault try ran");
                Equal(1, state.Other, "fault handler ran");
            }

            // Unsupported clause/control-flow shapes must remain correct by falling back to the interpreter.
            Equal(114, WasmJitEhFilterFallback(new ProbeException(114)), "filter fallback");
            state = new StateBox();
            Equal(15, WasmJitEhNestedFinallyFallback(state), "nested-finally fallback return");
            Equal(7, state.Value, "nested-finally fallback effects");
            state = new StateBox();
            Equal(16, WasmJitEhComplexFinallyFallback(state), "complex-finally fallback return");
            Equal(11, state.Value, "complex-finally fallback effects");

            // Re-enter already-published methods enough times to exercise repeated catch/unwind cleanup.
            for (int i = 0; i < 128; i++)
            {
                Equal(200 + i, WasmJitEhCatch(new ProbeException(200 + i)), "repeated local catch");
                Equal(300 + i, WasmJitEhCatchJitCallee(new ProbeException(300 + i)), "repeated JIT catch");
            }

            Console.WriteLine("WASM_JIT_EH_TEST_PASS");
            return 42;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"WASM_JIT_EH_TEST_FAIL: {ex}");
            return 1;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int WasmJitEhCatch(ProbeException exception)
    {
        try
        {
            throw exception;
        }
        catch (ProbeException ex)
        {
            return ex.Marker;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int WasmJitEhConditionalThrow(int value)
    {
        try
        {
            return checked(int.MaxValue + value);
        }
        catch (OverflowException)
        {
            return 102;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int WasmJitEhTypedCatch(ProbeException exception)
    {
        try
        {
            throw exception;
        }
        catch (ProbeException ex)
        {
            return ex.Marker;
        }
        catch (Exception)
        {
            return -1;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int WasmJitEhThrowLeaf(ProbeException exception) => throw exception;

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int WasmJitEhCatchJitCallee(ProbeException exception)
    {
        try
        {
            return WasmJitEhThrowLeaf(exception);
        }
        catch (ProbeException ex)
        {
            return ex.Marker;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int InterpThrowLeaf(ProbeException exception) => throw exception;

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int WasmJitEhCatchInterpCallee(ProbeException exception)
    {
        try
        {
            return InterpThrowLeaf(exception);
        }
        catch (ProbeException ex)
        {
            return ex.Marker;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int WasmJitEhCatchAcrossGc(ProbeException exception)
    {
        try
        {
            throw exception;
        }
        catch (ProbeException ex)
        {
            ForceGc();
            return ReferenceEquals(ex, exception) ? ex.Marker : -1;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void ForceGc()
    {
        for (int i = 0; i < 32; i++)
            _ = new byte[4096];
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void WasmJitEhRethrow(ProbeException exception)
    {
        try
        {
            throw exception;
        }
        catch (ProbeException)
        {
            throw;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void WasmJitEhCatchThrowsReplacement(ProbeException original, ProbeException replacement)
    {
        try
        {
            throw original;
        }
        catch (ProbeException)
        {
            throw replacement;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int WasmJitEhFinallyNormal(StateBox state)
    {
        try
        {
            state.Value = 1;
            return 7;
        }
        finally
        {
            state.Value += 2;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void WasmJitEhFinallyThrow(ProbeException exception, StateBox state)
    {
        try
        {
            throw exception;
        }
        finally
        {
            state.Value = 4;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void WasmJitEhFinallyReplacesException(
        ProbeException original,
        ProbeException replacement,
        StateBox state)
    {
        try
        {
            throw original;
        }
        finally
        {
            state.Value++;
            throw replacement;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int WasmJitEhCatchInsideNormalFinally(StateBox state)
    {
        try
        {
            return 9;
        }
        finally
        {
            try
            {
                throw new ProbeException(1);
            }
            catch (ProbeException)
            {
                state.Value = 5;
            }
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void WasmJitEhCatchInsideExceptionalFinally(ProbeException outer, StateBox state)
    {
        try
        {
            throw outer;
        }
        finally
        {
            try
            {
                throw new InvalidOperationException("nested");
            }
            catch (InvalidOperationException)
            {
                state.Value = 6;
            }
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void WasmJitEhInnerFinallyThrow(ProbeException exception, StateBox state)
    {
        try
        {
            throw exception;
        }
        finally
        {
            state.Value++;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int WasmJitEhOuterCatchInnerFinally(ProbeException exception, StateBox state)
    {
        try
        {
            WasmJitEhInnerFinallyThrow(exception, state);
            return -1;
        }
        catch (ProbeException ex)
        {
            return ex.Marker + state.Value;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void WasmJitEhMismatchedCatch(ProbeException exception)
    {
        try
        {
            throw exception;
        }
        catch (ArgumentException)
        {
            Fail("wrong catch selected");
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int WasmJitEhNullThrow()
    {
        try
        {
            throw null!;
        }
        catch (NullReferenceException)
        {
            return 112;
        }
    }

    private static Action<StateBox, ProbeException> CreateFaultMethod()
    {
        DynamicMethod method = new DynamicMethod(
            "WasmJitEhFault",
            typeof(void),
            new[] { typeof(StateBox), typeof(ProbeException) },
            typeof(WasmJitEhTests),
            skipVisibility: true);
        ILGenerator il = method.GetILGenerator();
        il.BeginExceptionBlock();
        il.Emit(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Ldc_I4_1);
        il.Emit(OpCodes.Stfld, typeof(StateBox).GetField(nameof(StateBox.Value))!);
        il.Emit(OpCodes.Ldarg_1);
        il.Emit(OpCodes.Throw);
        il.BeginFaultBlock();
        il.Emit(OpCodes.Ldarg_0);
        il.Emit(OpCodes.Ldc_I4_1);
        il.Emit(OpCodes.Stfld, typeof(StateBox).GetField(nameof(StateBox.Other))!);
        il.EndExceptionBlock();
        il.Emit(OpCodes.Ret);
        return method.CreateDelegate<Action<StateBox, ProbeException>>();
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int WasmJitEhFilterFallback(ProbeException exception)
    {
        try
        {
            throw exception;
        }
        catch (ProbeException ex) when (ex.Marker == 114)
        {
            return ex.Marker;
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int WasmJitEhNestedFinallyFallback(StateBox state)
    {
        try
        {
            return 15;
        }
        finally
        {
            state.Value += 1;
            try
            {
                state.Value += 2;
            }
            finally
            {
                state.Value += 4;
            }
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int WasmJitEhComplexFinallyFallback(StateBox state)
    {
        try
        {
            try
            {
                return 16;
            }
            finally
            {
                state.Value += 1;
            }
        }
        finally
        {
            state.Value += 10;
        }
    }

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
