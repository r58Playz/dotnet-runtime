/*
 * verdict.mjs -- ONE fault taxonomy and ONE run verdict, shared by every driver.
 *
 * WHY THIS EXISTS
 * ---------------
 * Failure classification was spread across six places that disagreed: mcdrive's four regexes,
 * bootcheck's own `isFail` (the only one that knew about `Uncaught (in promise)`), arm.sh's fifth
 * spelling in a `grep -oE`, mcab's `usable()`, and two copies of the void-window rule. The consequences
 * were not cosmetic:
 *
 *   * A run's `faults` said `[{kind:"fault"}]` whether the cause was a dangling MonoMethod
 *     (loader.c:1826), an uninitialised interp hash (mono-internal-hash.c:47) or a malformed EH clause
 *     table (method-to-ir.c:805). All three occurred in the archived corpus; all three read identically,
 *     so no batch could ever say which class a build had moved.
 *   * Whole families matched NOTHING: LinkError, CompileError, Aborted(), RangeError (both the
 *     WebAssembly.Memory and the call-stack spellings), unhandled promise rejections, renderer crashes.
 *     An uncounted route does not show up as a gap -- it shows up as everything else looking bigger.
 *   * mc-results.jsonl already holds at least four incompatible row schemas.
 *
 * THE RULE THIS FILE ENFORCES: classes are DISJOINT and every line gets at most one. One wasm trap emits
 * four log lines within ~8 ms (WORKER_TRAP_STACK, "worker sent an error!", two Uncaught RuntimeError), so
 * a pattern set that double-matches reports 4x the truth. R230 moved `memory access out of bounds` from
 * FAIL_RX into TRAP_RX rather than adding it alongside, for exactly this reason; the same discipline is
 * why the tables below are ORDERED and first-match-wins.
 *
 * VERIFY BEFORE TRUSTING. Five classifier defects have shipped in this tree, every one written from a
 * plausible mental model of the output and never checked against real output. `selftest()` at the bottom
 * runs the real signatures -- taken from archived logs, not invented -- and `faultcensus.mjs --selftest`
 * calls it. A class that finds ZERO in the archived corpus is a classifier bug until proven otherwise.
 */

/* ---------------------------------------------------------------------------------------------- *
 * Severity: what the DRIVER should do, which is not the same as how bad it is.
 *
 *   terminal  the run is over; waiting out the remaining budget cannot help. ACT on it.
 *   fault     counts against the build. The run may or may not continue.
 *   env       not ours: discard the run, do not count it. (R269: three distinct silent stalls occurred
 *             in one day and mcdrive reported "the runtime is wedged" for all three, which corrupts any
 *             stability gate built on its output.)
 * ---------------------------------------------------------------------------------------------- */
export const TERMINAL = "terminal", FAULT = "fault", ENV = "env";

const basename = (p) => String(p).replace(/^.*\//, "");

/*
 * ORDERED. First match wins, so the specific spellings come before the general ones.
 *
 * `key` is the point of the whole file: a STABLE, COMPARABLE identity for the class, so two batches can
 * be diffed. `mono-assert` keys on file:line because that is what names the bug; `wasm-trap` keys on the
 * RuntimeError text because that is what distinguishes a signature mismatch from an OOB.
 */
const RULES = [
  /* --- mono / native runtime ------------------------------------------------------------------- */
  {
    cls: "mono-abort", sev: TERMINAL,
    /* Emitted 3 s after the error that caused it, and it is the moment the process is gone. Matching it
     * is what stops one abort from wedging an entire batch: the browser dies, the driver keeps waiting,
     * and every remaining run of the batch is lost (observed at 886 s and 1,267 s). */
    rx: /MONO_WASM: forcing abort/,
    /* The abort line QUOTES whatever was last logged, and mono has two renderings:
     *   ... * Assertion at <file>:<line>, condition `<c>' not met     (g_assert)
     *   ... * Assertion: should not be reached at <file>:<line>        (g_assert_not_reached)
     * Read the file:line out of either, so the abort is keyed by the bug that caused it rather than by
     * the word "abort" -- which is the whole point of having a key. */
    key: (l) => (l.match(/Assertion(?:: should not be reached)? at \S*?([\w.-]+\.[ch]+):(\d+)/) || [])
                  .slice(1, 3).join(":") || "unknown",
  },
  {
    cls: "mono-assert", sev: FAULT,
    /* The real wire format, confirmed against archived logs:
     *   [MONO] * Assertion at /abs/path/mono/metadata/loader.c:1826, condition `...' not met
     * Note `[MONO]` is sometimes prefixed by a thread tag ([t9697e8e0][MONO:]), so anchor on "Assertion
     * at" and not on the bracket. */
    rx: /\* Assertion at .*?:\d+, condition/,
    /* `MONO_WASM: forcing abort ... [MONO] * Assertion at <file>:<line>` QUOTES the assertion that
     * caused it, so that one line matches this rule too. Ordering alone would resolve it, but then the
     * disjointness invariant could not be CHECKED -- and an invariant that cannot be checked is how
     * "one trap, four log lines, 4x the count" happened. Exclude it explicitly: the abort is the event,
     * and it already carries this assert's file:line as its own key. */
    not: /MONO_WASM: forcing abort/,
    key: (l) => { const m = l.match(/Assertion at (\S+?):(\d+),/); return m ? `${basename(m[1])}:${m[2]}` : "unknown"; },
  },
  {
    cls: "glib-assert", sev: FAULT,
    /* g_assert_not_reached / g_error / glib's own renderer. Distinct from mono-assert: no condition
     * text, and a different code path produced it. None of these matched any regex before. */
    rx: /\*\* ERROR:.*assertion failed|\* Assertion: should not be reached|assert_not_reached/,
    /* Same exclusion as mono-assert, and for the same reason: the abort line quotes this text verbatim. */
    not: /MONO_WASM: forcing abort/,
    /* `.h` AS WELL AS `.c`. The first version matched only `.c` and 113 of 115 real events keyed as
     * "unknown", because mono's g_assert_not_reached sites are overwhelmingly in headers --
     * sgen-scan-object.h:93 is the one this corpus actually carries. A key that is "unknown" for 98% of
     * a class is the same as having no key: two different bugs read as one bucket. */
    key: (l) => { const m = l.match(/([\w.-]+\.[ch]+):(\d+)/); return m ? `${m[1]}:${m[2]}` : "unknown"; },
  },

  {
    cls: "interp-native-sig", sev: TERMINAL,
    /* The interpreter reached a native function pointer (a calli: IKVM's JNI stubs, LWJGL's JNI.invoke*) whose
     * signature has no interp->native trampoline -- the build generates them per cookie. It is a g_error, and on a
     * WORKER that never ends the run: the logger's exit(1) does not stop the thread, g_error's eg_unreachable is a
     * for(;;), and the 3 s force-exit timer cannot run on a thread that is spinning. Unclassified, this read as
     * verdict=spin with faults=[] after the whole budget (R497). Keyed by the cookie, which names the shape. */
    rx: /CANNOT HANDLE INTERP ICALL SIG \w+/,
    not: /MONO_WASM: forcing abort/,
    key: (l) => (l.match(/CANNOT HANDLE INTERP ICALL SIG (\w+)/) || [, "unknown"])[1],
  },
  {
    cls: "gc-corrupt", sev: TERMINAL,
    /* sgen found an object whose header yields an impossible size while COPYING it (first seen p52 b1, 2026-09-26:
     *   [MONO] no object of size 3234331096d
     * from ms_find_block_obj_size_index <- major_alloc_object <- copy_object <- sgen_finalize_in_range <-
     * collect_nursery). That is heap corruption caught mid-collection: the collector never finishes, the world
     * stays stopped, and the run then reads as a SPIN (one thread busy, no app output) -- which is exactly how
     * the driver first reported it. Terminal: nothing after it is a measurement. */
    rx: /\bno object of size \d+/,
    key: () => "no object of size",
  },

  {
    cls: "ikvm-finish-failed", sev: FAULT,
    /* IKVM could not FINISH a dynamically generated type -- mono threw while building it (first seen p71 a2,
     * 2026-09-27, R350: `[IKVM finish-failed] net.minecraft.class_1950: System.TypeLoadException: VTable setup
     * of type net.minecraft.class_1950 failed`, which then crashed the Server thread and stopped the server).
     * Its own class so a recurrence is counted instead of folding into the generic server-died it causes. One
     * line in the whole archived corpus when this was added, so it is not routine IKVM control flow. Keyed on
     * the exception and its message head with the type name blanked, so two types failing one way read as one. */
    rx: /\[IKVM finish-failed\] \S+: [\w.]+Exception/,
    key: (l) => { const m = l.match(/\[IKVM finish-failed\] \S+: ([\w.]+): ([^"]{0,120})/);
                  return m ? `${m[1].split(".").pop()}: ${m[2].replace(/net\.minecraft\.\S+|\S+\.\S+\$\S*/g, "T").trim().slice(0, 48)}` : "unknown"; },
  },

  /* --- wasm ------------------------------------------------------------------------------------ */
  {
    cls: "wasm-link", sev: TERMINAL,
    /* A LinkError means the module never instantiated, so the method silently falls back to the
     * interpreter and the tier is partly or wholly dead -- which LOOKS LIKE A PERFORMANCE RESULT.
     * CLAUDE.md: "A wrong functype on an import fails *instantiation*, not the call". Terminal because
     * any timing taken afterwards is void. */
    /* Not Java's UnsatisfiedLinkError: 26.2's NativeLibrariesBootstrap.tryLoadingVulkan logs one on every boot and
     * falls back to GL. */
    rx: /(?<!Unsatisfied)LinkError|WebAssembly\.instantiate\(\).*Import #/,
    key: (l) => (l.match(/Import #\d+ [^:]*|LinkError:?\s*([^,;]{0,60})/) || [, "unknown"])[0],
  },
  {
    cls: "wasm-compile", sev: FAULT,
    /* "section was shorter than expected size" is this emitter's signature for an import count that has
     * fallen out of step with the name table. WINDOW_JS records that this exact error voided three A/Bs
     * while matching nothing. */
    rx: /CompileError|WebAssembly\.Module\(\):/,
    key: (l) => (l.match(/CompileError:?\s*(?:WebAssembly\.\w+\(\):\s*)?([^,@]{0,60})/) || [, "unknown"])[1]?.trim() || "unknown",
  },
  {
    cls: "js-stack", sev: FAULT,
    /* The admission DFS is recursive and explicitly unbounded in depth (WJC_ADMIT_DEPTH_MAX exists to
     * watch it), so this one is ours, not the browser's. Ordered before js-oom because V8 renders it as
     * a RangeError too. */
    rx: /Maximum call stack size exceeded|RangeError: Maximum call/,
    key: () => "call stack exceeded",
  },
  {
    cls: "wasm-trap", sev: FAULT,
    /* Kept DISJOINT from everything above, and `memory access out of bounds` lives here and nowhere
     * else. WORKER_TRAP_STACK exists only because of the local emsdk patch to worker.js /
     * library_pthread.js; it is the only way a WORKER's trap carries a stack, and it went unmatched by
     * anything for months. */
    rx: /function signature mismatch|memory access out of bounds|WORKER_TRAP_STACK|WORKER_REJECT_STACK|Uncaught RuntimeError|table index is out of bounds|unreachable executed|null function or function signature|does not support unaligned accesses/,
    /* WORKER_TRAP_STACK is a CHANNEL, not a cause -- it is the emsdk patch's header and it prefixes
     * whatever the worker actually threw. A stack overflow arrives as
     * `WORKER_TRAP_STACK RangeError: Maximum call stack size exceeded`, which this rule and js-stack
     * both match; js-stack names the bug and this one would key it "unknown", so js-stack is tested
     * first AND excluded here. Found on the corpus, not in the samples -- which is why --disjoint
     * checks the corpus. */
    not: /Maximum call stack size exceeded/,
    key: (l) => {
      const m = l.match(/(?:RuntimeError:\s*)?(function signature mismatch|memory access out of bounds|table index is out of bounds|unreachable executed|null function or function signature\w*|operation does not support unaligned accesses)/);
      return m ? m[1] : "unknown";
    },
  },

  /* --- js / browser ---------------------------------------------------------------------------- */
  {
    cls: "js-oom", sev: TERMINAL,
    rx: /V8 javascript OOM|Fatal JavaScript out of memory|FATAL ERROR: Reached heap limit|RangeError: WebAssembly\.Memory|Exceeding maximum wasm|Failed to reserve memory|Check failed: .*[Aa]llocat|PartitionAlloc.*(OOM|out of memory)/,
    key: (l) => (l.match(/V8 javascript OOM|Reached heap limit|WebAssembly\.Memory|Exceeding maximum wasm|Failed to reserve memory/) || [, ])[0] || "oom",
  },
  {
    cls: "em-abort", sev: TERMINAL,
    rx: /Aborted\(|__assert_fail|native code called abort\(\)/,
    key: (l) => (l.match(/Aborted\(([^)]{0,50})/) || [, "abort"])[1],
  },
  {
    cls: "renderer-crash", sev: TERMINAL,
    /* SIGILL was already matched (as browser-oom, oddly); SIGSEGV and chromium's own crash line were
     * not matched at all, so a renderer that died outright read as a silent wedge. */
    rx: /Received signal (4|11|6|7|8)\b|SIGILL|SIGSEGV|Renderer process crashed|\[FATAL:[^\]]*\]/,
    key: (l) => (l.match(/Received signal \d+|SIGILL|SIGSEGV|Renderer process crashed/) || [, "crash"])[0],
  },
  {
    cls: "unhandled-rejection", sev: FAULT,
    /* Existed only in bootcheck's private matcher; mcdrive never saw it. */
    rx: /Uncaught \(in promise\)/,
    /* A wasm trap can be DELIVERED as a rejected promise: `Uncaught (in promise) RuntimeError: function
     * signature mismatch` (27 in the corpus). The rejection is the channel; the trap is the cause, and
     * wasm-trap keys it by which trap. Ordering already picks the right one -- this makes it checkable. */
    not: /RuntimeError/,
    key: (l) => (l.match(/Uncaught \(in promise\)\s*([A-Za-z]*Error)/) || [, "rejection"])[1],
  },

  {
    cls: "linkage-error", sev: FAULT,
    /*
     * The java.lang.*Error linkage family. `mcdrive.mjs` has tracked these since R269 as
     * "RUNTIME/CODEGEN FAULT, not environment: COUNT IT" -- a linkage error means the bytes we generated
     * do not link, which is ours -- but they had NO CLASS HERE, so the census could not see them at all.
     * Found while tracing an `indy-bootstrap`: its 2026-08-13 instance bottoms out in
     * `NoSuchMethodError: java.lang.invoke.MethodHandleImpl.fillArray(...)` via
     * InnerClassLambdaMetafactory.buildCallSite, i.e. a missing arity overload -- a completely different
     * root cause from the 2026-09-21 instance, and one nothing would have counted.
     *
     * Ordered BEFORE indy-bootstrap so the specific cause wins over the generic wrapper: Fabric reports
     * both as `BootstrapMethodError: call site initialization exception`, and that wrapper names the
     * symptom, not the cause.
     */
    rx: /java\.lang\.(NoSuchMethodError|NoSuchFieldError|NoClassDefFoundError|IllegalAccessError|VerifyError|LinkageError|IncompatibleClassChangeError|AbstractMethodError|ClassFormatError|ClassCircularityError|UnsatisfiedLinkError)/,
    /* 26.2's NativeLibrariesBootstrap.tryLoadingVulkan probes libvulkan on every boot and falls back to GL */
    not: /UnsatisfiedLinkError: Failed to locate library: libvulkan/,
    key: (l) => {
      const m = l.match(/java\.lang\.(\w+Error)(?::\s*([\w.$]+))?/);
      if (!m) return "linkage";
      /* Key on the error AND the member it names, because "NoSuchMethodError" alone merges unrelated
       * bugs -- the whole reason mono-assert keys on file:line rather than on "assert". */
      return m[2] ? `${m[1]}:${m[2].split("(")[0].slice(-48)}` : m[1];
    },
  },

  {
    cls: "indy-bootstrap", sev: FAULT,
    /*
     * AN INVOKEDYNAMIC BOOTSTRAP THAT DID NOT PRODUCE A CallSite. Ours, and specifically the IKVM
     * lazy-site / MethodHandle machinery -- the observed chain runs through
     * `cli.System.Object.DMH.invokeStaticInit_L_V`, i.e. IKVM's DirectMethodHandle path, which is exactly
     * the `__<>lazySite$indy$<key>` bridge population R276-R280 worked on.
     *
     * IT WAS INVISIBLE TO EVERY MATCHER, including the first version of this file: only TWO occurrences
     * exist across the whole archived corpus (2026-08-13 and 2026-09-21), and the second was initially
     * classified `env` because Fabric reports it and Fabric is what the driver saw. See the ordering note
     * in classifyRun: FABRIC IS THE REPORTER, NOT THE CAUSE.
     */
    rx: /BootstrapMethodError|bootstrap method failed to produce a CallSite|call site initialization exception/,
    key: (l) => /failed to produce a CallSite/.test(l) ? "no CallSite" : "bootstrap",
  },

  /* --- managed / game -------------------------------------------------------------------------- */
  {
    cls: "managed-fault", sev: FAULT,
    /* What FAIL_RX meant once the runtime-level spellings above were MOVED out of it rather than
     * duplicated. Game-logic and managed-cast failures only. */
    rx: /unexpected GC filler class|Arg_InvalidCastException|Unable to cast object of type|cannot be cast to|Failed to light chunk|Exception ticking world|Lock is no longer valid|Game crashed!|MD5 MessageDigest|Unreported exception thrown/,
    /* `mono_class_get_flags: unexpected GC filler class` arrives INSIDE a mono assertion:
     *   * Assertion at .../class-accessors.c:90, condition `0' not met, function:mono_class_get_flags,
     *     mono_class_get_flags: unexpected GC filler class
     * That is not a managed cast failure -- it is the third documented face of the dangling-MonoMethod*
     * class (CLAUDE.md lists it alongside the OOB in jit_mm_for_method), and it must key as
     * class-accessors.c:90 so it can be counted against that fix. */
    not: /\* Assertion(?:: should not be reached)? at |MONO_WASM: forcing abort/,
    key: (l) => (l.match(/unexpected GC filler class|Arg_InvalidCastException|cannot be cast to|Failed to light chunk|Exception ticking world|Lock is no longer valid|Game crashed!|MD5 MessageDigest|Unreported exception thrown/) || [, "managed"])[0],
  },

  /* --- environment: recorded, never counted ---------------------------------------------------- */
  {
    cls: "net-timeout", sev: ENV,
    /* MCSR Ranked's anti-cheat during Fabric preLaunch. The runtime is healthy and correctly propagated
     * what the mod threw; R269 lost an OBJGUARD arm to reading this as a wedge. */
    rx: /TimeoutException: connect took too long|SocketTimeoutException|UnknownHostException/,
    key: () => "network",
  },
  {
    cls: "void-env", sev: ENV,
    /* No usable GL context: emscripten-glfw dies at glfwCreateWindow before the title screen. */
    rx: /GLFW error/,
    key: (l) => (l.match(/GLFW error \d+/) || [, "GLFW error"])[0],
  },
];

/*
 * A STACK FRAME IS NOT AN EVENT, and this cost a 10x over-count on its first census run.
 *
 * mono renders an assertion as a message line followed by its own backtrace, and one of those frames is
 * literally `at dotnet.native.wasm.monoeg_assertion_message (...)`. Matching a SYMBOL NAME as if it were
 * a MESSAGE turned every assert into ~10 events: 115 glib-assert events over 10 logs, of which 112 were
 * frames belonging to the 3 real ones. Identical in shape to the `-turbofan` suffix defect CLAUDE.md
 * records, and to the four other classifier bugs before it -- a pattern written from a plausible mental
 * model of the output, never checked against the output.
 *
 * Frames are `<ws>at <symbol> (<url>:<line>)`, after chromium's `[pid:tid:MMDD/...:LEVEL:CONSOLE:n] "`
 * prefix and any mono thread tag. They are kept in the run record as the fault's STACK -- that is where
 * they belong -- but they never open an event of their own.
 */
const CLASSLOAD_RX = /\[IkvmClassLoader\]|loaded from URLClassLoader/;

export const STACK_FRAME_RX = /^(?:\[[^\]]*\]\s*)*"?\s+at [\w$.<>-]+\s*[({]/;

/** Classify one log line. Returns `{cls, key, sev}` or null. First match wins; classes are disjoint. */
export function classifyLine (line) {
  if (!line) return null;
  if (STACK_FRAME_RX.test(line)) return null;
  /*
   * A CLASS BEING LOADED IS NOT A CLASS BEING THROWN. IKVM logs EVERY class it loads:
   *   [IkvmClassLoader] 'net.fabricmc.loader.impl.FormattedException' loaded from URLClassLoader
   * so any rule that matches a bare exception TYPE NAME fires ~30 s into a perfectly healthy boot.
   * bootcheck.mjs learned this the expensive way -- a first cut scored FIVE ARMS "FAIL" on exactly that
   * line and produced no information at all -- and its fix never made it into the shared patterns.
   *
   * Measured: 132,216 class-load lines across 80 large logs currently produce ZERO false positives here,
   * because every rule above anchors on a MESSAGE SHAPE rather than a type name. So this guard changes
   * nothing today; it is here so that the next rule someone adds cannot quietly reintroduce the defect,
   * and the selftest case below is what keeps it honest.
   */
  if (CLASSLOAD_RX.test(line)) return null;
  /* NOR IS A CLASS BEING PINNED. MONO_GC_DEBUG=print-pinning logs the class of every pinned object
   * (`GC_PIN_CLASS:      1  stack:0 static:0 other:1  java.lang.NoSuchMethodError`), and linkage-error DOES
   * match a bare type name -- so the claim above no longer holds for it. It scored all four p23 runs as
   * `linkage-error:NoSuchMethodError` (R300). */
  if (/\bGC_PIN_CLASS:/.test(line)) return null;
  /* NOR IS A METHOD BEING COUNTED OR CAPTURED. The JIT tier's tab-separated census records name methods in mono's
   * `Class:member` form -- MONO_WASM_JIT_SITE_COUNT's `WASM_JIT_SITE\t...\tvoid java.lang.LinkageError:.ctor (string)`
   * and MONO_WASM_JIT_CAPTURE's `WASM_JIT_CAPTURE_BEGIN\t...` -- so every census and ledger mapping run scored
   * `linkage-error:LinkageError:.ctor` (R519 saw the capture case; 2026-11-05's census and mapping runs both). Only
   * these two records, matched with their TAB: the space-separated WASM_JIT_* records (ADMIT_FAIL, VSLOW_FAIL,
   * WILD_STORE, ...) report real faults and stay classified. */
  if (/WASM_JIT_(?:SITE|CAPTURE_BEGIN)\t/.test(line)) return null;
  for (const r of RULES) {
    if (!r.rx.test(line)) continue;
    if (r.not && r.not.test(line)) continue;
    let key = "unknown";
    try { key = r.key(line) || "unknown"; } catch { }
    return { cls: r.cls, key: String(key).slice(0, 80), sev: r.sev };
  }
  return null;
}

/* Exported ONLY so faultcensus --disjoint can run the invariant over the real corpus. Not for matching:
 * use classifyLine, which applies the ordering and the `not` exclusions together. */
export const RULES_FOR_TEST = RULES;

/** Every class this file can emit, so a census can print a zero row rather than silently omitting it. */
export const ALL_CLASSES = RULES.map((r) => r.cls);
export const SEVERITY_OF = Object.fromEntries(RULES.map((r) => [r.cls, r.sev]));

/* ---------------------------------------------------------------------------------------------- *
 * THE RUN VERDICT -- one field, replacing six ad-hoc derivations.
 *
 * The three EXCLUDED verdicts are R269's finding promoted from log prose into data: three distinct
 * silent-stall causes occurred in one day and only one was a JIT fault, while the driver asserted "the
 * runtime is wedged" for all three.
 * ---------------------------------------------------------------------------------------------- */
export const VERDICTS = {
  ok:            { counts: true,  clean: true  },
  fault:         { counts: true,  clean: false },   // a fault-severity class fired
  wedge:         { counts: true,  clean: false },   // no progress, threads ASLEEP
  spin:          { counts: true,  clean: false },   // no progress, threads RUNNING -- R275's livelock
  timeout:       { counts: true,  clean: false },   // budget exhausted while still making some progress
  env:           { counts: false, clean: false },   // network / mod-loader abort / no GL
  "never-started": { counts: false, clean: false }, // PreInit never entered: page-load or browser failure
  killed:        { counts: false, clean: false },   // browser+driver vanished together: external cleanup
};
export const COUNTS_AGAINST_BUILD = (v) => !!VERDICTS[v]?.counts;

/**
 * Decide one run's verdict from what the driver observed.
 *
 * Ordering is the whole content of this function, and it is deliberate: the EXCLUSIONS are tested first,
 * because every one of them can also produce a fault line or a stall, and counting those against a build
 * is what corrupts a stability gate.
 *
 * `worldGenMode` is NOT an input. It is world-generation DURATION (the documented ~20%-incidence
 * bimodality, 30 s vs 172 s), not a failure -- a `mode=stall` run routinely completes with faults=[] and
 * ok=true. This session has already conflated it with the world-load hang once.
 */
export function classifyRun ({ marks, faults = [], reachedEnd = false, stalled = false, spin = null,
                               browserGone = false, timedOut = false, envAbort = null } = {}) {
  const has = (m) => (marks?.has ? marks.has(m) : !!marks?.[m]);
  const sev = new Set(faults.map((f) => f.sev || SEVERITY_OF[f.cls]));

  /* Externally killed: the browser AND the driver vanish together with no stall message and no crash
   * artifact. CLAUDE.md names the harness's own cleanup as this exact triple, and the run's chromium is
   * adopted into a KDE app-org.chromium.Chromium-<pid>.scope under app.slice -- i.e. into the user
   * session rather than the measurement's cgroup -- so session-level cleanup reaches it even when the
   * driver is protected by a transient unit. Not a workload failure. */
  if (browserGone && !has("preinit")) return "killed";

  /* The app never started: PreInit never entered, so the runtime never ran and cannot have wedged.
   * Observed as an 8-line log holding only chromium's GCM/portal warnings with the dev server serving
   * 200 throughout -- and, twice, as a full /tmp. */
  if (!has("preinit")) return "never-started";

  /*
   * `envAbort` is the stall classifier's OWN finding -- a Fabric/entrypoint abort or a network timeout in
   * the log -- passed in rather than re-derived, so the narrative the driver PRINTS and the verdict it
   * RECORDS cannot disagree. They did: the soak's one genuine wedge printed "NOT a runtime wedge ...
   * treat this run as an ENVIRONMENT failure and discard it" while recording `verdict=wedge`. A reader
   * gets the prose; a census gets the field; one of them was wrong and nothing flagged it.
   */
  /*
   * FAULTS ARE TESTED BEFORE ENVIRONMENT, and the order is the whole point.
   *
   * Fabric catches an entrypoint exception, puts up its error GUI and logs nothing further -- so "Fabric
   * aborted" is what the driver SEES for both a genuine environment failure (a network timeout) and a
   * defect of ours. The 20-run soak's one failure was
   *   FormattedException: BootstrapMethodError: call site initialization exception
   *     Caused by: ClassCastException: bootstrap method failed to produce a CallSite
   * i.e. an invokedynamic bootstrap in IKVM's own MethodHandle path -- and the first version of this
   * function returned `env` for it, because envAbort was tested first. That discards our own bug as
   * somebody else's.
   *
   * mcdrive's own comment already says it: "the outermost wrapper names the symptom, not the cause".
   * So: if the log carries a FAULT-severity class, that is the answer regardless of who reported it.
   * `net-timeout` is ENV severity and therefore still reaches the env arm below.
   */
  if (sev.has(TERMINAL)) return "fault";
  if (sev.has(FAULT)) return "fault";

  if (envAbort && !has("inworld")) return "env";
  if (sev.has(ENV) && !has("inworld")) return "env";

  /* Spin vs wedge. Same symptom, opposite mechanisms, and R275 had to discover the difference by hand:
   * "It is not blocked ... every previous description of this hang as 'stopped' or 'blocked' was wrong
   * about the mechanism; it is a busy spin." They need different next instruments, so they are different
   * verdicts. */
  if (spin?.spinning) return "spin";
  if (stalled) return "wedge";
  if (timedOut) return "timeout";
  return reachedEnd ? "ok" : "timeout";
}

/** Per-class counts + first occurrence, for the run record and for a batch census. */
export function summariseFaults (faults = []) {
  const out = {};
  for (const f of faults) {
    const k = `${f.cls}:${f.key}`;
    if (!out[k]) out[k] = { cls: f.cls, key: f.key, sev: f.sev || SEVERITY_OF[f.cls], n: 0, firstAt: f.at ?? null };
    out[k].n += f.lines || 1;
  }
  return out;
}

/* ---------------------------------------------------------------------------------------------- *
 * SELFTEST. Every string below was taken from a real archived log or from the source that emits it --
 * none was invented. `expect: null` lines are the negative controls that prove the disjointness claim
 * and that a healthy line is not classified as a fault.
 * ---------------------------------------------------------------------------------------------- */
const CASES = [
  ["[MONO] * Assertion: should not be reached at /home/r/src/mono/mono/sgen/sgen-scan-object.h:93", "glib-assert", "sgen-scan-object.h:93"],
  ["[0x78ba07b8-back] MONO_WASM: forcing abort 3000ms after last error log message [MONO] * Assertion: should not be reached at /x/mono/sgen/sgen-scan-object.h:93", "mono-abort", "sgen-scan-object.h:93"],
  ['"[MONO] * Assertion at /home/r/src/mono/mono/metadata/loader.c:1826, condition `mono_metadata_token_table (m->token) == MONO_TABLE_METHOD\' not met', "mono-assert", "loader.c:1826"],
  ['[t9697e8e0][MONO:] * Assertion at /home/r/src/mono/mono/utils/mono-internal-hash.c:47, condition `table->table != NULL\' not met', "mono-assert", "mono-internal-hash.c:47"],
  ['[MONO] * Assertion at /home/r/src/mono/mono/mini/method-to-ir.c:805, condition `bb\' not met', "mono-assert", "method-to-ir.c:805"],
  ['[0x9697e8e0-norm] MONO_WASM: forcing abort 3000ms after last error log message [MONO] * Assertion at /x/mono-internal-hash.c:47, condition', "mono-abort", "mono-internal-hash.c:47"],
  ['[144550:144550:1016/002815.977338:INFO:CONSOLE:3] "[MONO] CANNOT HANDLE INTERP ICALL SIG LIILIIIIIIII', "interp-native-sig", "LIILIIIIIIII"],
  ['"[t11d6db88][MONO:] CANNOT HANDLE INTERP ICALL SIG LIILIIIIIIII', "interp-native-sig", "LIILIIIIIIII"],
  ["WORKER_TRAP_STACK RuntimeError: memory access out of bounds", "wasm-trap", "memory access out of bounds"],
  ["Uncaught RuntimeError: function signature mismatch", "wasm-trap", "function signature mismatch"],
  ["RuntimeError: operation does not support unaligned accesses at a_cas.8", "wasm-trap", "operation does not support unaligned accesses"],
  ["RuntimeError: table index is out of bounds", "wasm-trap", "table index is out of bounds"],
  ["Uncaught LinkError: WebAssembly.instantiate(): Import #3 module=\"s\" error", "wasm-link", null],
  ['"java.lang.UnsatisfiedLinkError: Failed to locate library: libvulkan.so.1", source: https://localhost:5021/src/dotnet/log.ts (13)', null, null],
  ["CompileError: WebAssembly.Module(): section was shorter than expected size", "wasm-compile", null],
  ["FATAL ERROR: Reached heap limit Allocation failed", "js-oom", null],
  ["RangeError: WebAssembly.Memory(): could not allocate memory", "js-oom", null],
  ["v8_initializer.cc:971] V8 javascript OOM (CALL_AND_RETRY_LAST).", "js-oom", null],
  ["RangeError: Maximum call stack size exceeded", "js-stack", "call stack exceeded"],
  ["Aborted(native code called abort())", "em-abort", null],
  ["Received signal 11 SEGV_MAPERR", "renderer-crash", null],
  ["Uncaught (in promise) TypeError: cannot read", "unhandled-rejection", "TypeError"],
  ["** ERROR:mini-wasm.c:100: assertion failed: (x != NULL)", "glib-assert", "mini-wasm.c:100"],
  ['[t628c5980][MONO:] no object of size 3234331096d', "gc-corrupt", "no object of size"],
  ['"[IKVM finish-failed] net.minecraft.class_1950: System.TypeLoadException: VTable setup of type net.minecraft.class_1950 failed", source: https://localhost:5021/src/dotnet/log.ts (13)', "ikvm-finish-failed", "TypeLoadException: VTable setup of type T failed"],
  ["java.lang.ClassCastException: cannot be cast to net.minecraft.class_2", "managed-fault", null],
  ["Uncaught (in promise) RuntimeError: function signature mismatch", "wasm-trap", "function signature mismatch"],
  ["[MONO] * Assertion at /x/mono/metadata/class-accessors.c:90, condition `0' not met, function:mono_class_get_flags, mono_class_get_flags: unexpected GC filler class", "mono-assert", "class-accessors.c:90"],
  ["net.fabricmc.loader.impl.FormattedException: java.lang.BootstrapMethodError: call site initialization exception", "indy-bootstrap", "bootstrap"],
  ["Caused by: java.lang.NoSuchMethodError: java.lang.invoke.MethodHandleImpl.fillArray(Ljava/lang/Integer;[Ljava/lang/Object;)Ljava/lang/Object;", "linkage-error", "NoSuchMethodError:java.lang.invoke.MethodHandleImpl.fillArray"],
  ["java.lang.IllegalAccessError: tried to access class foo.Bar", "linkage-error", "IllegalAccessError:tried"],
  ["Caused by: java.lang.ClassCastException: bootstrap method failed to produce a CallSite", "indy-bootstrap", "no CallSite"],
  ["cli.System.TimeoutException: connect took too long", "net-timeout", "network"],
  ["GLFW error 65544: Failed to create window", "void-env", "GLFW error 65544"],
  /* --- negative controls: healthy lines that must NOT classify --- */
  ['[123:123:0919/132207.3:INFO:CONSOLE:3] "    at dotnet.native.wasm.monoeg_assertion_message (https://x/dotnet.native.wasm:wasm-function[413901]:0x5ef8f51)', null, null],
  ["    at wasm_jit_ethunk_cb (wasm://wasm/dotnet.native.wasm-2d1c3fae:wasm-function[9]:0x1234)", null, null],
  ["[IkvmClassLoader] 'net.fabricmc.loader.impl.FormattedException' loaded from URLClassLoader", null, null],
  ["[IkvmClassLoader] 'java.lang.ClassCastException' loaded from URLClassLoader", null, null],
  ["[t32a8c008][MONO:Mono] GC_PIN_CLASS:      1  stack:0      static:0      other:1       java.lang.NoSuchMethodError", null, null],
  ["[MONO] GC_PIN_CLASS:      3  stack:3      static:0      other:0       java.lang.ClassCastException", null, null],
  ['[473752:473752:1105/151130.516966:INFO:CONSOLE:13] "WASM_JIT_SITE\t1\t187\t0\td\t4\t0x19f16cb0\t0x19671438\tvoid java.lang.LinkageError:.ctor (string)\t-\tjava.lang.Error:.ctor\t-\t-1", source: https://localhost:5021/src/dotnet/log.ts (13)', null, null],
  ['[466745:466745:1105/145950.719218:INFO:CONSOLE:13] "WASM_JIT_CAPTURE_BEGIN\t17906\tm1\t652\tvoid java.lang.IncompatibleClassChangeError:.ctor (string)", source: https://localhost:5021/src/dotnet/log.ts (13)', null, null],
  ["[wasm-jit stats] registered=32057 bailed=12 invalid=0", null, null],
  ["Preparing spawn area: 83%", null, null],
  ["[bench] fps=14.3 frames=1820", null, null],
  ["Stopping singleplayer server", null, null],   // a CLEAN quit emits this; it is not a fault
  ["gcm/registration_request failed", null, null],
  ["[wasm-jit badeh] declined=0 bad_offsets=0", null, null],
  ["[MONO] GC: no objects of size class 3 to sweep", null, null],   // prose that merely resembles gc-corrupt
];

export function selftest ({ log = console.log } = {}) {
  let bad = 0;
  for (const [line, wantCls, wantKey] of CASES) {
    const got = classifyLine(line);
    const gotCls = got?.cls ?? null;
    if (gotCls !== wantCls) { log(`  FAIL class: ${JSON.stringify(line.slice(0, 60))} -> ${gotCls}, want ${wantCls}`); bad++; continue; }
    if (wantKey != null && got.key !== wantKey) { log(`  FAIL key:   ${JSON.stringify(line.slice(0, 60))} -> ${got.key}, want ${wantKey}`); bad++; }
  }
  /* Disjointness is a property of the RULE SET, not of these samples: assert no two rules claim the same
   * line. Checked by re-running every case against every rule. */
  for (const [line] of CASES) {
    const hits = RULES.filter((r) => r.rx.test(line) && !(r.not && r.not.test(line))).map((r) => r.cls);
    if (hits.length > 1) { log(`  FAIL disjoint: ${JSON.stringify(line.slice(0, 60))} matches ${hits.join(", ")}`); bad++; }
  }
  log(bad ? `verdict selftest: ${bad} FAILURE(S) over ${CASES.length} cases` : `verdict selftest: ${CASES.length} cases OK, rules disjoint`);
  return bad === 0;
}
