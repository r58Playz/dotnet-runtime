// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Exercises the wasm-JIT GC-reference model under nursery churn and full collections:
// refs held in (write-through) locals across allocating callees and explicit GCs, deref-base
// temporaries (slot-elision candidates), loop-carried refs across safepoints, ref/byref args,
// and short-lived refs that die before a GC point (dead-slot zeroing candidates). Run with the
// pin-pressure flags in every combination (MONO_WASM_JIT_REF_WT / SLOTLIVE / SLOTZERO): a missing
// or stale slot mirror shows up as a dangling reference after the copying nursery moves the
// object — the checks below then fail loudly instead of corrupting silently.

using System;
using System.Runtime.CompilerServices;

public static class WasmJitGcRefTests
{
    public sealed class Node
    {
        public int Value;
        public Node Next;
        public string Tag;
    }

    public static int Main()
    {
        try
        {
            // Warm TryGetValue first: name-targeted methods compile on first call, and GcRefByref
            // must see its callee already JITted — a residual call with a byref arg is a clean
            // whole-method bail, which would defeat the byref coverage this probe exists for.
            TryGetValue(0, out int warm);
            if (warm != 5)
                Fail($"TryGetValue warm-up: got {warm}");

            for (int round = 0; round < 64; round++)
            {
                Equal(3 * round + 3, GcRefHoldAcrossCalls(round), "refs live across allocating calls + GC");
                Equal(round + 41, GcRefDerefBases(round), "deref-base temporaries");
                int n = (round & 3) + 3;
                Equal(n * (n - 1), GcRefLoopCarried(round), "loop-carried refs across safepoints");
                Equal(77 + round, GcRefArgsPinned(new Node { Value = 77 + round, Tag = "arg" }), "ref arg pinned across GC");
                Equal(round + 5, GcRefByref(round), "byref out-arg across GC");
                Equal(round + 9, GcRefDeadSlots(round), "dead refs before a GC point");
                string s = GcRefStrings(round.ToString());
                if (s != "tag-" + round + "-x")
                    Fail($"string refs across GC: got '{s}'");
            }

            Console.WriteLine("WASM_JIT_GCREF_TEST_PASS");
            return 42;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"WASM_JIT_GCREF_TEST_FAIL: {ex}");
            return 1;
        }
    }

    // Interp-resident helpers (not in the JIT target list): calls to them are interp residuals, so
    // the caller's refs must survive the residual's own allocations and collections.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Churn(int n)
    {
        for (int i = 0; i < 8 * n; i++)
            _ = new byte[512];
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void ForceGc()
    {
        Churn(4);
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static Node MakeNode(int value, Node next)
    {
        Churn(1);
        return new Node { Value = value, Next = next, Tag = "n" + value };
    }

    // Multiple refs held in locals across allocating callees and two full GCs. If any local's pin
    // mirror is missing or stale, the nursery copy moves and a.Value/b.Value read freed memory.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int GcRefHoldAcrossCalls(int r)
    {
        Node a = MakeNode(r, null);
        Node b = MakeNode(r + 1, a);
        ForceGc();
        Churn(4);
        Node c = MakeNode(r + 2, b);
        ForceGc();
        if (!ReferenceEquals(c.Next, b) || !ReferenceEquals(b.Next, a))
            return -1;
        return a.Value + b.Value + c.Value;
    }

    // Deref-base temporaries consumed immediately, with no GC point between definition and use —
    // the exact population SLOTLIVE elides. The values must still be correct.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int GcRefDerefBases(int r)
    {
        Node n = MakeNode(r + 41, null);
        int v = n.Value;
        int len = n.Tag.Length;
        return len > 0 ? v : -1;
    }

    // A loop-carried ref (head) redefined and used across per-iteration allocations and GCs —
    // must keep its slot (use-before-def / cross-generation), and the chain must stay intact.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int GcRefLoopCarried(int r)
    {
        Node head = null;
        int sum = 0;
        int n = (r & 3) + 3;
        for (int i = 0; i < n; i++)
        {
            head = MakeNode(i, head);
            if ((i & 1) == 0)
                ForceGc();
            sum += head.Value;
        }
        for (Node p = head; p != null; p = p.Next)
            sum += p.Value;
        return sum;
    }

    // A reference ARG must be pinned by the callee's prologue slot copy across a GC.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int GcRefArgsPinned(Node arg)
    {
        ForceGc();
        Churn(2);
        return arg.Value;
    }

    // The callee holds a byref (a managed pointer into the caller's frame) AND a ref local across a
    // GC, then writes through the byref. NB: the out-param is a SCALAR — an `out Node` would force
    // the caller to take the address of a ref local, a shape the emitter deliberately bails on.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static bool TryGetValue(int r, out int value)
    {
        Churn(1);
        Node n = MakeNode(r + 5, null);
        ForceGc();
        value = n.Value;
        return true;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int GcRefByref(int r)
    {
        if (!TryGetValue(r, out int v))
            return -1;
        ForceGc();
        return v;
    }

    // Short-lived refs whose last use precedes a GC point in the same block — SLOTZERO zeroes
    // their slots right after that last use; the still-live value must be unaffected.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int GcRefDeadSlots(int r)
    {
        Node t1 = MakeNode(r, null);
        int v1 = t1.Value;
        Node t2 = MakeNode(v1 + 9, null);
        int v2 = t2.Value;
        ForceGc();
        return v2 - r + r;
    }

    // String refs (heap objects produced by corlib calls) across collections. Takes the round
    // number pre-stringified: `"tag-" + r` on an int arg lowers to r.ToString() via ldarga, a
    // shape the emitter bails on — string+string concat stays a plain call.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static string GcRefStrings(string rs)
    {
        string a = "tag-" + rs;
        ForceGc();
        string b = a + "-x";
        Churn(2);
        ForceGc();
        return b;
    }

    private static void Equal(int expected, int actual, string path)
    {
        if (expected != actual)
            Fail($"{path}: expected {expected}, got {actual}");
    }

    private static void Fail(string message) => throw new Exception(message);
}
