// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Collections.Generic;
using System.Collections.Specialized;
using System.IO;
using System.Threading.Tasks;
using System.Linq;
using Xunit;
using Xunit.Abstractions;

#nullable enable

namespace Wasm.Build.Tests
{
    public class WasmBuildAppTest : WasmBuildAppBase
    {
        // similar to MainWithArgsTests.cs, consider merging
        public WasmBuildAppTest(ITestOutputHelper output, SharedBuildPerTestClassFixture buildContext) : base(output, buildContext)
        {}

        [Theory]
        [MemberData(nameof(MainMethodTestData), parameters: new object[] { /*aot*/ true })]
        [MemberData(nameof(MainMethodTestData), parameters: new object[] { /*aot*/ false })]
        public async Task TopLevelMain(Configuration config, bool aot)
            => await TestMain("top_level",
                    @"System.Console.WriteLine(""Hello, World!""); return await System.Threading.Tasks.Task.FromResult(42);",
                    config, aot);

        [Theory]
        [MemberData(nameof(MainMethodTestData), parameters: new object[] { /*aot*/ true })]
        [MemberData(nameof(MainMethodTestData), parameters: new object[] { /*aot*/ false })]
        public async Task AsyncMain(Configuration config, bool aot)
            => await TestMain("async_main", @"
            using System;
            using System.Threading.Tasks;

            public class TestClass {
                public static async Task<int> Main()
                {
                    Console.WriteLine(""Hello, World!"");
                    return await Task.FromResult(42);
                }
            }", config, aot);

        [Theory]
        [MemberData(nameof(MainMethodTestData), parameters: new object[] { /*aot*/ true })]
        [MemberData(nameof(MainMethodTestData), parameters: new object[] { /*aot*/ false })]
        public async Task NonAsyncMain(Configuration config, bool aot)
            => await TestMain("non_async_main", @"
                using System;
                using System.Threading.Tasks;

                public class TestClass {
                    public static int Main()
                    {
                        Console.WriteLine(""Hello, World!"");
                        return 42;
                    }
                }", config, aot);

        [Theory]
        [MemberData(nameof(MainMethodTestData), parameters: new object[] { /*aot*/ false })]
        public async Task ExceptionFromMain(Configuration config, bool aot)
            => await TestMain("main_exception", """
                using System;
                using System.Threading.Tasks;

                public class TestClass {
                    public static int Main() => throw new Exception("MessageFromMyException");
                }
                """, config, aot, expectedExitCode: 1, expectedOutput: "Error: MessageFromMyException");

        private static string s_bug49588_ProgramCS = @"
            using System;
            public class TestClass {
                public static int Main()
                {
                    Console.WriteLine($""tc: {Environment.TickCount}, tc64: {Environment.TickCount64}"");

                    // if this gets printed, then we didn't crash!
                    Console.WriteLine(""Hello, World!"");
                    return 42;
                }
            }";

        [Theory]
        [MemberData(nameof(MainMethodTestData), parameters: new object[] { /*aot*/ true })]
        public async Task Bug49588_RegressionTest_AOT(Configuration config, bool aot)
            => await TestMain("bug49588_aot", s_bug49588_ProgramCS, config, aot);

        [Theory]
        [MemberData(nameof(MainMethodTestData), parameters: new object[] { /*aot*/ false })]
        public async Task Bug49588_RegressionTest_NativeRelinking(Configuration config, bool aot)
            => await TestMain("bug49588_native_relinking", s_bug49588_ProgramCS, config, aot,
                        extraArgs: "-p:WasmBuildNative=true",
                        isNativeBuild: true);

        [Theory]
        [BuildAndRun(config: Configuration.Release, aot: false)]
        public async Task WasmJitExceptionHandling(Configuration config, bool aot)
        {
            const string targetedMethods =
                "WasmJitEhCatch,WasmJitEhConditionalThrow,WasmJitEhTypedCatch," +
                "WasmJitEhThrowLeaf,WasmJitEhCatchJitCallee,WasmJitEhCatchInterpCallee," +
                "WasmJitEhCatchAcrossGc,WasmJitEhRethrow,WasmJitEhCatchThrowsReplacement," +
                "WasmJitEhFinallyNormal,WasmJitEhFinallyThrow,WasmJitEhFinallyReplacesException," +
                "WasmJitEhCatchInsideNormalFinally," +
                "WasmJitEhCatchInsideExceptionalFinally,WasmJitEhInnerFinallyThrow," +
                "WasmJitEhOuterCatchInnerFinally,WasmJitEhMismatchedCatch,WasmJitEhNullThrow," +
                "WasmJitEhFault,WasmJitEhFilterFallback,WasmJitEhNestedFinallyFallback," +
                "WasmJitEhComplexFinallyFallback";

            ProjectInfo info = CopyTestAsset(config, aot, TestAsset.WasmBasicTestApp, "wasm_jit_eh");
            ReplaceFile(
                Path.Combine("Common", "Program.cs"),
                Path.Combine(BuildEnvironment.TestAssetsPath, "EntryPoints", "WasmJitEh.cs"));
            PublishProject(
                info,
                config,
                new PublishOptions(AOT: false, ExtraMSBuildArgs: "-p:WasmBuildNative=true"),
                isNativeBuild: true);

            NameValueCollection query = new()
            {
                ["MONO_WASM_JIT_METHOD"] = targetedMethods,
                ["MONO_WASM_JIT_VERBOSE"] = "2",
                ["MONO_WASM_JIT_STATS"] = "1",
            };
            RunResult result = await RunForPublishWithWebServer(new BrowserRunOptions(
                config,
                TestScenario: "WasmJitEhTest",
                BrowserQueryString: query,
                ExpectedExitCode: 42));

            Assert.Contains(result.ConsoleOutput, line => line.Contains("WASM_JIT_EH_TEST_PASS"));

            string[] nativeEhMethods =
            {
                "WasmJitEhCatch", "WasmJitEhTypedCatch",
                "WasmJitEhThrowLeaf", "WasmJitEhCatchJitCallee", "WasmJitEhCatchInterpCallee",
                "WasmJitEhCatchAcrossGc", "WasmJitEhRethrow", "WasmJitEhCatchThrowsReplacement",
                "WasmJitEhFinallyNormal", "WasmJitEhFinallyThrow", "WasmJitEhFinallyReplacesException",
                "WasmJitEhCatchInsideNormalFinally",
                "WasmJitEhCatchInsideExceptionalFinally", "WasmJitEhInnerFinallyThrow",
                "WasmJitEhOuterCatchInnerFinally", "WasmJitEhMismatchedCatch", "WasmJitEhNullThrow",
                "WasmJitEhFault",
            };
            foreach (string method in nativeEhMethods)
                Assert.Contains(result.ConsoleOutput, line =>
                    line.Contains("WASM_JIT_REGISTERED") && line.Contains($"WasmJitEhTests:{method}"));

            // checked addition currently lowers to int_addcc, which the wasm-JIT does not support. Keep
            // this as a non-EH-opcode fallback guard: its catch still has to execute correctly in interp.
            Assert.Contains(result.ConsoleOutput, line =>
                line.Contains("WASM_JIT_BAIL") && line.Contains("WasmJitEhTests:WasmJitEhConditionalThrow") &&
                line.Contains("int_addcc"));
            Assert.Contains(result.ConsoleOutput, line =>
                line.Contains("WASM_JIT_BAIL") && line.Contains("WasmJitEhTests:WasmJitEhFilterFallback") &&
                line.Contains("filter not supported"));
            Assert.Contains(result.ConsoleOutput, line =>
                line.Contains("WASM_JIT_BAIL") && line.Contains("WasmJitEhTests:WasmJitEhNestedFinallyFallback") &&
                line.Contains("nested finally inside finally handler"));
            Assert.Contains(result.ConsoleOutput, line =>
                line.Contains("WASM_JIT_BAIL") && line.Contains("WasmJitEhTests:WasmJitEhComplexFinallyFallback") &&
                line.Contains("complex call_handler continuation"));
        }

        [Theory]
        [BuildAndRun(config: Configuration.Release, aot: false)]
        public async Task WasmJitValueTypeAbi(Configuration config, bool aot)
        {
            // Every probe in the asset, so name-targeting compiles them on first call. VtFilteredTake /
            // VtFilteredMake carry EH filters -> perm-bail; with RESIDUAL_PERM=1 their JITted callers
            // route those edges through the interp residual (the by-addr / vret scratch marshal path).
            const string targetedMethods =
                "VtTakeMHA,VtMutateMHA,VtCallMHA,VtMakeMHA,VtUseMakeMHA," +
                "VtFilteredTake,VtFilteredMake,VtResidualTake,VtResidualMake," +
                "VtAcrossGc,VtSumTags,VtGcInside,VtTakeScalar,VtTakeScalarRef,VtMakeScalar,VtMakeScalarRef," +
                "VtUseScalarReturn,VtUseScalarRefReturn,VtFilteredScalarReturn,VtResidualScalarReturn,VtVirtualScalarArg,VtVirtualTake,VtVirtualByaddrArg,VtVirtualTakeMha,VtMixedOffsets," +
                "VtTakeNested,VtCallNested,VtRefLocal,VtConsumeRef," +
                "VtInvokeMhaDelegate,VtInvokeIntDelegate,VtDelegateAdd,VtDelegateDouble," +
                "VtTakeScalarLong,VtMakeScalarLong,VtUseScalarLongReturn," +
                "VtTakeScalarDouble,VtMakeScalarDouble,VtUseScalarDoubleReturn," +
                "VtBoundRefBias,VtBoundIntBias";

            ProjectInfo info = CopyTestAsset(config, aot, TestAsset.WasmBasicTestApp, "wasm_jit_vtype");
            ReplaceFile(
                Path.Combine("Common", "Program.cs"),
                Path.Combine(BuildEnvironment.TestAssetsPath, "EntryPoints", "WasmJitVtype.cs"));
            PublishProject(
                info,
                config,
                new PublishOptions(AOT: false, ExtraMSBuildArgs: "-p:WasmBuildNative=true"),
                isNativeBuild: true);

            NameValueCollection query = new()
            {
                ["MONO_WASM_JIT_METHOD"] = targetedMethods,
                ["MONO_WASM_JIT_VERBOSE"] = "2",
                ["MONO_WASM_JIT_STATS"] = "1",
                ["MONO_WASM_JIT_RESIDUAL_PERM"] = "1",
            };
            RunResult result = await RunForPublishWithWebServer(new BrowserRunOptions(
                config,
                TestScenario: "WasmJitEhTest",
                BrowserQueryString: query,
                ExpectedExitCode: 42));

            Assert.Contains(result.ConsoleOutput, line => line.Contains("WASM_JIT_VTYPE_TEST_PASS"));

            // by-addr arg + hidden-vret shapes must actually JIT (not silently bail to the interp)
            string[] vtypeAbiMethods =
            {
                "VtTakeMHA", "VtMutateMHA", "VtCallMHA", "VtMakeMHA", "VtUseMakeMHA",
                "VtResidualTake", "VtResidualMake", "VtAcrossGc", "VtSumTags",
                "VtGcInside", "VtTakeScalar", "VtTakeScalarRef", "VtMakeScalar", "VtMakeScalarRef",
                "VtUseScalarReturn", "VtUseScalarRefReturn", "VtResidualScalarReturn", "VtVirtualScalarArg", "VtVirtualTake", "VtVirtualByaddrArg", "VtVirtualTakeMha", "VtMixedOffsets",
                "VtTakeNested", "VtCallNested", "VtRefLocal", "VtConsumeRef",
                "VtInvokeMhaDelegate", "VtInvokeIntDelegate", "VtDelegateDouble",
                // wide-etype (i64/f64) scalar-vtype shapes
                "VtTakeScalarLong", "VtMakeScalarLong", "VtUseScalarLongReturn",
                "VtTakeScalarDouble", "VtMakeScalarDouble", "VtUseScalarDoubleReturn",
                // bound-static ref-arg delegate target: compiled by the direct-target recipe's
                // maybe_compile in wasm_jit_prepare_delegate_call (VtBoundIntBias is intentionally
                // NOT asserted: its value-type bound arg stays on the wrapper path)
                "VtBoundRefBias",
            };
            foreach (string method in vtypeAbiMethods)
                Assert.Contains(result.ConsoleOutput, line =>
                    line.Contains("WASM_JIT_REGISTERED") && line.Contains($"WasmJitVtypeTests:{method}"));

            // the EH-filter probes stay interp-resident (perm bail) — that is what forces the
            // residual by-addr / vret marshal in their JITted callers
            Assert.Contains(result.ConsoleOutput, line =>
                line.Contains("WASM_JIT_BAIL") && line.Contains("WasmJitVtypeTests:VtFilteredTake") &&
                line.Contains("filter not supported"));
            Assert.Contains(result.ConsoleOutput, line =>
                line.Contains("WASM_JIT_BAIL") && line.Contains("WasmJitVtypeTests:VtFilteredMake") &&
                line.Contains("filter not supported"));
            Assert.Contains(result.ConsoleOutput, line =>
                line.Contains("WASM_JIT_BAIL") && line.Contains("WasmJitVtypeTests:VtFilteredScalarReturn") &&
                line.Contains("filter not supported"));
        }

        [Theory]
        [BuildAndRun(config: Configuration.Release, aot: false)]
        public async Task WasmJitGcRefPinning(Configuration config, bool aot)
        {
            // GC-reference model soak (see WasmJitGcRef.cs): refs held across allocating residuals +
            // full collections, deref-base temporaries, loop-carried refs, ref/byref args, dead refs.
            // Runs the same publish under every pin-pressure flag combination — write-through locals
            // (REF_WT), GC-point slot elision (SLOTLIVE), dead-slot zeroing (SLOTZERO) — plus
            // REFVERIFY=1 so a lost classification seed logs instead of corrupting silently.
            const string targetedMethods =
                "GcRefHoldAcrossCalls,GcRefDerefBases,GcRefLoopCarried,GcRefArgsPinned," +
                "GcRefByref,GcRefDeadSlots,GcRefStrings,MakeNode,TryGetValue";

            ProjectInfo info = CopyTestAsset(config, aot, TestAsset.WasmBasicTestApp, "wasm_jit_gcref");
            ReplaceFile(
                Path.Combine("Common", "Program.cs"),
                Path.Combine(BuildEnvironment.TestAssetsPath, "EntryPoints", "WasmJitGcRef.cs"));
            PublishProject(
                info,
                config,
                new PublishOptions(AOT: false, ExtraMSBuildArgs: "-p:WasmBuildNative=true"),
                isNativeBuild: true);

            var flagCombos = new (string RefWt, string SlotLive, string SlotZero)[]
            {
                ("0", "0", "0"),   // baseline (slot-homed, all slots)
                ("1", "0", "0"),   // write-through only
                ("1", "1", "0"),   // + slot elision
                ("1", "1", "1"),   // + dead-slot zeroing
            };

            foreach (var combo in flagCombos)
            {
                NameValueCollection query = new()
                {
                    ["MONO_WASM_JIT_METHOD"] = targetedMethods,
                    ["MONO_WASM_JIT_VERBOSE"] = "2",
                    ["MONO_WASM_JIT_STATS"] = "1",
                    ["MONO_WASM_JIT_REFVERIFY"] = "1",
                    ["MONO_WASM_JIT_REF_WT"] = combo.RefWt,
                    ["MONO_WASM_JIT_SLOTLIVE"] = combo.SlotLive,
                    ["MONO_WASM_JIT_SLOTZERO"] = combo.SlotZero,
                };
                RunResult result = await RunForPublishWithWebServer(new BrowserRunOptions(
                    config,
                    TestScenario: "WasmJitEhTest",
                    BrowserQueryString: query,
                    ExpectedExitCode: 42));

                Assert.Contains(result.ConsoleOutput, line => line.Contains("WASM_JIT_GCREF_TEST_PASS"));

                // The GcRef* probes must actually JIT (not silently bail to the interp) for the run to
                // prove anything. MakeNode/TryGetNode stay targeted but unasserted — whether their
                // allocation shapes JIT or bail, the callers' ref handling is what's under test.
                string[] mustRegister =
                {
                    "GcRefHoldAcrossCalls", "GcRefDerefBases", "GcRefLoopCarried", "GcRefArgsPinned",
                    "GcRefByref", "GcRefDeadSlots", "GcRefStrings",
                };
                foreach (string method in mustRegister)
                    Assert.Contains(result.ConsoleOutput, line =>
                        line.Contains("WASM_JIT_REGISTERED") && line.Contains($"WasmJitGcRefTests:{method}"));

                // a lost classification seed (would-be silent corruption) must not appear
                Assert.DoesNotContain(result.ConsoleOutput, line => line.Contains("WASM_JIT_REFVERIFY:"));
            }
        }

        [Theory]
        [BuildAndRun(config: Configuration.Release, aot: true)]
        public async Task WasmJitAotExceptionBoundary(Configuration config, bool aot)
        {
            const string targetedMethods =
                "WasmJitEhAotThrowToJitCatch,WasmJitEhJitThrowToAotCatch," +
                "WasmJitEhAotThrowToJitRethrow,WasmJitEhJitFinallyToAotCatch";

            ProjectInfo info = CopyTestAsset(config, aot, TestAsset.WasmBasicTestApp, "wasm_jit_aot_eh_boundary");
            ReplaceFile(
                Path.Combine("Common", "Program.cs"),
                Path.Combine(BuildEnvironment.TestAssetsPath, "EntryPoints", "WasmJitEhAotBoundary.cs"));
            PublishProject(info, config, new PublishOptions(AOT: true), isNativeBuild: true);

            NameValueCollection query = new()
            {
                ["MONO_WASM_JIT_METHOD"] = targetedMethods,
                ["MONO_WASM_JIT_VERBOSE"] = "2",
                ["MONO_WASM_JIT_STATS"] = "1",
                ["MONO_LOG_LEVEL"] = "debug",
                ["MONO_LOG_MASK"] = "aot",
            };
            RunResult result = await RunForPublishWithWebServer(new BrowserRunOptions(
                config,
                AOT: true,
                TestScenario: "WasmJitEhTest",
                BrowserQueryString: query,
                ExpectedExitCode: 42));

            Assert.Contains(result.ConsoleOutput, line => line.Contains("WASM_JIT_AOT_EH_BOUNDARY_PASS"));
            Assert.Contains(result.ConsoleOutput, line =>
                line.Contains("Found statically linked AOT module 'WasmBasicTestApp'"));

            foreach (string method in targetedMethods.Split(','))
                Assert.Contains(result.ConsoleOutput, line =>
                    line.Contains("WASM_JIT_REGISTERED") && line.Contains($"WasmJitEhAotBoundaryTests:{method}"));
        }

        [Theory]
        [BuildAndRun]
        [ActiveIssue("https://github.com/dotnet/runtime/issues/97449")]
        public async Task PropertiesFromRuntimeConfigJson(Configuration config, bool aot)
            => await TestMain("runtime_config_json",
                        @"
                        using System;
                        using System.Runtime.CompilerServices;

                        var config = AppContext.GetData(""test_runtimeconfig_json"");
                        Console.WriteLine ($""test_runtimeconfig_json: {(string)config}"");
                        return 42;
                        ",
                        config,
                        aot,
                        runtimeConfigContents: @"
                            },
                            ""configProperties"": {
                            ""abc"": ""4"",
                            ""test_runtimeconfig_json"": ""25""
                            }
                        }",
                        expectedOutput: "test_runtimeconfig_json: 25");

        [Theory]
        [BuildAndRun]
        [ActiveIssue("https://github.com/dotnet/runtime/issues/97449")]
        public async Task PropertiesFromCsproj(Configuration config, bool aot)
            => await TestMain("csproj_properties",
                        @"
                        using System;
                        using System.Runtime.CompilerServices;

                        var config = AppContext.GetData(""System.Threading.ThreadPool.MaxThreads"");
                        Console.WriteLine ($""System.Threading.ThreadPool.MaxThreads: {(string)config}"");
                        return 42;
                        ",
                        config,
                        aot,
                        extraArgs: "-p:ThreadPoolMaxThreads=20",
                        expectedOutput: "System.Threading.ThreadPool.MaxThreads: 20");
    }
}
