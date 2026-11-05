#!/usr/bin/env python3
"""bootattr.py <mcperf-outdir> [--show CLASS] [--top N] [--seg NAME] [--role ROLE] [--why|--what]

Boot attribution for one mcperf capture (R487). Splits boot (navigate -> FAILED_AUTH) into FINE segments taken from
Minecraft's own log lines, names each renderer thread's ROLE, and attributes every task-clock sample twice:

  WHY  -- the activity the sample is part of, from its CALL CHAIN: a fixed priority list of anchors (GC beats JIT
          compile beats interp transform ...), the highest-priority anchor found anywhere in the chain wins. Answers
          "what would go away if X were free".
  WHAT -- what the LEAF instruction is executing (JIT-emitted Java, interpreter, AOT managed, mono C, malloc, V8, ...).

threads.tsv (boot/threadsamp.py) supplies the OFF-CPU half: per thread and segment, run / runqueue / sleep seconds and
the wchan it sleeps in. Requires a task-clock (frequency-mode) capture: samples are then proportional to CPU time.

CHECK THE CLASSIFIER BEFORE QUOTING IT: `--show <class>` lists the symbols a class holds; `unclassified` is printed.
"""
import collections, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
os.environ.setdefault("PERFRAW_DSO_SYMS", "1")
import perfraw  # noqa: E402

# ---------------------------------------------------------------------------------------------------------------
# Fine markers (first match). Read off r484final/f1's log, 2026-10-08. `navigate` comes from phases.json.
FINE = [
    ("preinit", r'"PreInit\.\.\."'),
    ("ikvmReady", r'"dotnet initialized"'),
    ("javaMain", r'"Run\.\.\."'),
    ("mods", r"Loading \d+ mods:"),
    ("mixinExtras", r"Initializing MixinExtras"),
    ("userSet", r"Setting user:"),
    ("reload", r"Reloading ResourceManager"),
    ("titleState", r"StateOutput State: title"),
    ("atlasDone", r"mob_effects\.png-atlas"),
    # the MCSR line, then 1.16.5 (ModernFix measure_time) and 26.2 (RRLS) -- lib/seeds.mjs titleRx
    ("titleReady", r"Update Status: FAILED_AUTH|Overlay is removed!|Game took [\d.]+ seconds to start"),
    ("datapack", r"Loaded \d+ recipes|Loaded \d+ advancements"),
    ("generating", r"StateOutput State: generating|Preparing spawn area: \d+%|Preparing start region for dimension"),
    ("spawnDone", r"Time elapsed: \d+ ms"),
    ("inworld", r"StateOutput State: inworld|joined the game"),
]
SEGMENTS = [
    ("1 page+runtime", "navigate", "preinit", "page load, fetch + compile dotnet.native.wasm, mono init"),
    ("2 IKVM init", "preinit", "ikvmReady", "IkvmWasm.PreInit"),
    ("3 Fabric boot", "javaMain", "mods", "Knot launcher, mod discovery"),
    ("4 Mixin setup", "mods", "mixinExtras", "mixin configs, preLaunch entrypoints"),
    ("5 main() classload", "mixinExtras", "userSet", "game classes loaded + mixin-transformed until Main sets the user"),
    ("6 client init", "userSet", "reload", "MinecraftClient.<init>: options, window/GL, registries"),
    ("7 resource reload", "reload", "atlasDone", "resource packs, models, textures, sounds (loading overlay)"),
    ("8 title screen", "atlasDone", "titleReady", "overlay fade, RankedMainScreen, auth attempt"),
]
WORLD = [("world: datapack+server", "datapack", "generating", ""), ("world: generate", "generating", "spawnDone", ""),
         ("world: join", "spawnDone", "inworld", "")]
TS = re.compile(rb"^\[\d+:\d+:\d{4}/(\d\d)(\d\d)(\d\d)\.(\d+):")


def log_marks(path):
    pend = [(n, re.compile(r.encode())) for n, r in FINE]
    out = {}
    with open(path, "rb") as f:
        for line in f:
            m = TS.match(line)
            if not m:
                continue
            for i, (n, rx) in enumerate(pend):
                if rx.search(line):
                    out[n] = int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]) + int(m[4]) / 1e6
                    pend.pop(i)
                    break
    return out


def marks_mono(d, phases):
    """Fine marks on the monotonic clock. The driver's own marks (phases.json) fix the log->monotonic offset; it
    is checked against every marker the two share, and a disagreement over 50 ms is reported."""
    lm = log_marks(os.path.join(d, "game.log"))
    drv = {}
    for p in phases["phases"]:
        n = p["name"]
        for nm, frm, to in (("boot", "navigate", "titleReady"), ("boot.classload", "javaMain", "userSet"),
                            ("boot.resources", "userSet", "titleState"), ("boot.ikvminit", "preinit", "ikvmReady"),
                            ("world.gen", "generating", "spawnDone"), ("world.join", "spawnDone", "inworld")):
            if n == nm:
                drv[frm], drv[to] = p["t0"], p["t1"]
    offs = sorted(drv[k] - lm[k] for k in drv if k in lm)
    off = offs[len(offs) // 2]
    if offs[-1] - offs[0] > 0.05:
        print(f"!! log->monotonic offsets disagree by {1e3 * (offs[-1] - offs[0]):.0f} ms across {len(offs)} marks")
    mono = {k: v + off for k, v in lm.items()}
    mono["navigate"] = drv["navigate"]
    # MCSR's last atlas line has no counterpart on other seeds: segment 7 then runs to the title, segment 8 is empty
    if "atlasDone" not in mono and "titleReady" in mono:
        mono["atlasDone"] = mono["titleReady"]
    return mono


# ---------------------------------------------------------------------------------------------------------------
# COMPONENT: walk the chain from the LEAF upward; the first frame that is an ACTIVITY ANCHOR or an EXECUTION frame
# decides. Neutral frames (malloc, libc, locks, hash tables, chromium plumbing, corlib) are skipped, so a hash lookup
# inside type loading is type loading and an allocation inside interpreted Java is interpreted Java. Stopping at the
# first EXECUTION frame matters: IKVM's class-loader frames sit ABOVE the Java ClassLoader code they call (Knot,
# Mixin/ASM), and anchoring on any frame in the chain credited all of that Java to IKVM (the first cut of R487 did).
ANCHORS = [
    ("clock: mono_100ns_ticks -> clock_gettime -> performance.now()", r"^JS:(mono_100ns_ticks|mono_msec_ticks|__clock_gettime|"
                            r"minipal_(lowres|hires)_ticks|emscripten_get_now|clock_gettime)-"),
    ("V8: Liftoff frame setup builtin", r"^Builtin:WasmLiftoffFrameSetup"),
    ("JIT<->interp boundary (entry/exit, thunks, imethod lookup)", r"^JS:(wj_call_interp|interp_entry|do_jit_call|wj_vslow_stub|"
                            r"mono_wasm_jit_invoke_caught|mono_interp_get_imethod|mono_interp_peek_imethod|stackval_from_data|"
                            r"stackval_to_data|mono_jiterp_interp_entry|mono_wasm_jit_scratch|wasm_jit_maybe_compile|"
                            r"interp_create_method_pointer|mono_jiterp_placeholder)"),
    ("GC: sgen collection", r"^JS:(sgen_perform_collection|sgen_ensure_free_space|collect_nursery|major_do_collection|"
                            r"major_start_collection|major_finish_collection|sgen_gc_collect|mono_gc_collect|sgen_stop_world|"
                            r"sgen_client_stop_world|sgen_restart_world|sgen_workers_|sgen_major_collector|sgen_collect_)"),
    ("JIT tier: V8 compile+instantiate of emitted modules", r"^JS:(mono_wasm_jit_instantiate|wasm_jit_instantiate|wj_instantiate|"
                            r"mono_wasm_jit_validate|wasm_module_lazy_bank|mono_wasm_jit_ensure_bank|wj_lazy_ensure_bank)"),
    ("JIT tier: mono compile (IR passes + emit)", r"^JS:(mini_method_compile|mono_wasm_emit_method|wj_assemble|mono_wasm_jit_compile|"
                            r"wasm_jit_compile|wj_reemit_drain_one|wj_compile|mono_wasm_jit_try_compile|mono_method_to_ir|"
                            r"mono_optimize_branches|mono_compile_dominator_info|mono_ssa_|mono_local_|mono_spill|mono_decompose|"
                            r"mono_handle_global_vregs|mono_analyze_liveness|mono_compute_natural_loops|mono_abcrem|mono_if_conversion|"
                            r"mono_insert_safepoints|mono_wasm_emit|wasm_emit_)"),
    ("interp: transform (IL -> interp code)", r"^JS:(mono_interp_transform_method|generate_code|interp_transform_|do_transform_method|"
                            r"interp_optimize_code|interp_alloc_offsets|interp_fold_|interp_cprop|interp_super_instructions)"),
    ("V8: lazy compile of main-module functions", r"^Builtin:WasmCompileLazy|^Builtin:WasmTriggerTierUp"),
    ("exceptions: throw / unwind / stack trace", r"^JS:(mono_handle_exception|mono_wasm_throw|mono_llvm_throw|mono_llvm_rethrow|__cxa_throw|"
                            r"mono_get_frame_info|ves_icall_System_Diagnostics_StackFrame|ves_icall_System_Diagnostics_StackTrace|"
                            r"mono_walk_stack|mono_arch_unwind_frame|unwinder_unwind_frame|mono_find_jit_info|mono_llvm_raise|"
                            r"mono_llvm_resume|mono_set_pending_exception|mono_error_prepare_exception|mono_exception_from|"
                            r"IKVM_Runtime_.*ExceptionHelper|IKVM_Java_java_lang_Throwable_fillInStackTrace|IKVM_Java_java_lang_Throwable__ctor|"
                            r"corlib_System_Diagnostics_StackTrace|corlib_System_Exception_)"),
    ("AOT: lazy method init / resolution", r"^JS:(mono_aot_init_llvm_method|init_method|mono_aot_get_method|mono_aot_get_class_from_name|"
                            r"mono_aot_init_gshared|mono_aot_plt_resolve|load_method|mono_resolve_patch_target|decode_method_ref|"
                            r"decode_klass_ref|mono_aot_get_unbox|mono_aot_find_jit_info)-"),
    ("IKVM: Java bytecode -> IL (Compiler/CodeEmitter)", r"^JS:IKVM_Runtime_IKVM_Runtime_(Compiler|CodeEmitter|MethodAnalyzer|"
                            r"ClassFile_Method_Code|ClassFile_Method_Instruction|LocalVarInfo|InstructionState|Intrinsics|"
                            r"ExceptionTableEntry|BranchInfo|UntangledExceptionTable)"),
    ("IKVM/mono: Reflection.Emit + type bake", r"^JS:(.*System_Reflection_Emit_|mono_reflection_|ves_icall_.*(TypeBuilder|ModuleBuilder|"
                            r"AssemblyBuilder|DynamicMethod|RuntimeMethodBuilder|RuntimeTypeBuilder|RuntimeModuleBuilder|"
                            r"RuntimeAssemblyBuilder|RuntimeILGenerator)|mono_image_basic_init|mono_dynamic_image|mono_image_create_|"
                            r"mono_image_build_metadata|mono_reflection_|reflection_|mono_type_get_object|mono_method_get_object|"
                            r"mono_field_get_object|IKVM_Runtime_IKVM_Runtime_.*(TypeBuilder|DynamicTypeWrapper|JavaTypeImpl_Finish|"
                            r"_Finish|FinishCore|DefineMethod|DefineField|Emit))"),
    ("IKVM: class file parse / define / link (managed)", r"^JS:IKVM_Runtime_"),
    ("mono: type/metadata loading", r"^JS:(mono_class_init|mono_class_setup_|mono_class_from_name|mono_class_get_checked|mono_class_create_|"
                            r"mono_metadata_parse_|mono_class_inflate|mono_class_vtable|mono_class_layout|mono_class_from_typeref|"
                            r"mono_assembly_load|mono_image_open|mono_class_get_field|mono_method_get_header|mono_get_method_checked|"
                            r"mono_runtime_class_init|mono_class_from_mono_type|mono_metadata_|mono_class_|mono_method_signature|"
                            r"mono_field_|mono_type_|mono_method_get_|mono_get_method|mono_signature|mono_class_implement|"
                            r"mono_custom_attrs|mono_generic_class|mono_method_desc|mono_lookup_|inflate_generic|mono_image_)"),
    ("JIT tier: dispatch/admission helpers", r"^JS:(mono_wasm_jit_admit|vcall_resolve_fslot|mono_wasm_jit_late_fslot|wj_lazy_bind|"
                            r"mono_wasm_jit_vcall|wj_vcall_|wj_delegate_|mono_wasm_jit_delegate|wj_prof_|mono_wasm_jit_safepoint)"),
    ("GL: mesa-wink", r"^JS:(wink_|gex_wgpu|_mesa_|st_|vbo_|glthread|nir_|u_upload|tint|wgpu|emwgpu|_gl[A-Z]|gl[A-Z]|spirv|vtn_|"
                      r"ralloc|util_queue|lp_|draw_|gallivm|cso_|pipe_)"),
]
ANCHORS = [(n, re.compile(p.encode())) for n, p in ANCHORS]
EXEC = [
    ("exec: JIT-emitted Java (our tier)", r"^JS:[^ ]+ [^ ]+:[^ ]+ \(|^JS:wj_[a-z_]*stub|^JS:wj_lazy_bank|^JS:wasm-function\["),
    ("exec: interpreter", r"^JS:(mono_interp_exec_method|interp_exec_method)-"),
    ("exec: AOT JDK (IKVM.Java)", r"^JS:IKVM_Java_"),
    ("exec: zlib/png", r"^JS:(inflate|adler32|crc32|png_|deflate)"),
]
EXEC = [(n, re.compile(p.encode())) for n, p in EXEC]
JS_GLUE = re.compile(rb"^JS:[~*]|^JS:\S*https?://|^JS:\S*\.m?js:")
NEUTRAL_CORLIB = re.compile(rb"^JS:(corlib_|System_|aot_wrapper|.*_wrapper_)")

# WHAT: the leaf. Ordered; first match wins.
CHROME_WHAT = [
    ("V8: memory.grow -> update every instance", rb"SetInstanceMemory|WasmMemoryObject::UpdateInstances|WasmMemoryObject::Grow|"
                                                 rb"GrowMemory|BackingStore::"),
    ("V8: wasm compile (Liftoff/TurboFan/validate)", rb"wasm::(Liftoff|.*Compil|.*Decoder|WasmFullDecoder|.*Turboshaft|"
                                                   rb".*Validat|ModuleDecoder|.*Tier|ValidateFunctionBody)|compiler::|"
                                                   rb"wasm::.*Code(Space|Manager|Allocator)|Assembler::|MacroAssembler|"
                                                   rb"wasm::NativeModule|wasm::WasmCode"),
    ("V8: wasm instantiate (instances, tables, wrappers)", rb"InstanceBuilder|WasmTableObject|UpdateDispatchTable|WasmInstanceObject|"
                                                   rb"WasmTrustedInstanceData|ImportWrapper|WasmDispatchTable|wasm::.*Wrapper"),
    ("V8: GC", rb"Mark|Sweep|Scaveng|Heap::|Evacuat|Remembered|SlotSet|ConcurrentMarking|MemoryChunk|cppgc::|heap::base::|"
               rb"MinorMS|GCTracer|Worklist"),
    ("V8: other", rb"v8::|v8_inspector|Builtins_"),
    ("PartitionAlloc", rb"partition_alloc|allocator_shim"),
    ("Blink/WebGPU/GPU IPC", rb"blink::|dawn|wgpu|gpu::|viz::|cc::|mojo::|IPC::"),
    ("chromium other", rb""),
]
CHROME_WHAT = [(n, re.compile(p)) for n, p in CHROME_WHAT]
WHAT = [
    ("V8 builtin: WasmLiftoffFrameSetup", r"^Builtin:WasmLiftoffFrameSetup"),
    ("V8 builtin: other (glue, ICs, interp JS)", r"^(Builtin:|BytecodeHandler:|Stub:|JS:wasm-to-js|JS:js-to-wasm|RegExp:)"),
    ("JS (page/emscripten glue)", r"^JS:[~*]|^JS:\S*https?://|^JS:\S*\.m?js:"),
    ("JIT-emitted Java (our tier)", r"^JS:[^ ]+ [^ ]+:[^ ]+ \(|^JS:wj_[a-z_]*stub|^JS:wj_lazy_bank|^JS:wasm-function\["),
    ("GL: mesa-wink (wasm)", r"^JS:(wink_|gex_wgpu|_mesa_|st_|vbo_|glthread|nir_|util_|u_upload|tint|wgpu|emwgpu|_gl[A-Z]|gl[A-Z]|spirv|vtn_|ralloc)"),
    ("zlib/png (wasm)", r"^JS:(inflate|adler32|crc32|png_|deflate|zcalloc)"),
    ("interp: exec loop", r"^JS:(mono_interp_exec_method|interp_entry|interp_exec_method|do_jit_call|stackval_|interp_runtime_invoke|"
                          r"mono_interp_|interp_)"),
    ("AOT managed: IKVM.Runtime", r"^JS:IKVM_Runtime_"),
    ("AOT managed: IKVM.Java (JDK)", r"^JS:IKVM_Java_"),
    ("AOT managed: corlib/System", r"^JS:(corlib_|System_|aot_wrapper|.*_wrapper_)"),
    ("JIT tier helpers (C)", r"^JS:(mono_wasm_jit_|wj_|wasm_jit_|get_virtual_method_fast|wasm_enc|wasm_emit)"),
    ("malloc/free", r"^JS:(dlmalloc|dlfree|dlrealloc|dlcalloc|internal_memalign|dlmemalign|malloc|free|sbrk|emscripten_builtin_|"
                    r"tmalloc|prepend_alloc|sys_alloc|release_unused|dispose_chunk|mono_mempool_|g_malloc|monoeg_malloc|monoeg_g_free|g_free)"),
    ("locks/futex/threads (wasm)", r"^JS:(__pthread_|pthread_|emscripten_futex|__wait|__wake|_emscripten_|emscripten_|a_cas|"
                                   r"__lock|__unlock|mono_os_|mono_coop_|mono_thread|mono_threads_)"),
    ("sgen (alloc + GC work)", r"^JS:(sgen_|major_|minor_|simple_nursery|drain_gray|mono_gc_|mono_sgen|pin_|scan_|copy_object|"
                               r"major_copy|mono_gchandle|mono_array_new|mono_object_new|mono_string_new|ves_icall.*alloc)"),
    ("mono runtime C (other)", r"^JS:(mono_|mini_|ves_icall|monoeg_|g_|dn_|eg_|do_|decode_|load_|interp|emit_|mono|"
                               r"jit_|method_|class_|type_|ves_)"),
    ("libc (wasm)", r"^JS:(mem|str|__|qsort|bsearch|wcs|sn?printf|vsn?printf|fmt_|isw?|to(lower|upper)|atoi|strto)"),
    ("native libc", r"^DSO:/usr/lib/(libc|ld-linux|libm|libstdc)"),
    ("native other", r"^DSO:|^\[unknown\]|^\[vdso\]"),
]
WHAT = [(n, re.compile(p.encode())) for n, p in WHAT]


def what_of(leaf):
    if leaf.startswith(b"DSO:/usr/lib/chromium/chromium"):
        fn = leaf.split(b"!", 1)[1] if b"!" in leaf else b""
        for n, rx in CHROME_WHAT:
            if rx.search(fn):
                return n
    for n, rx in WHAT:
        if rx.search(leaf):
            return n
    return "unclassified"


def component_of(names):
    """names: leaf first. Innermost anchor or execution frame; else the leaf's WHAT."""
    corlib = js = False
    for nm in names:
        if JS_GLUE.search(nm):      # a JS import's body: belongs to the wasm frame that called it, if any
            js = True
            continue
        for n, rx in ANCHORS:
            if rx.search(nm):
                return n
        for n, rx in EXEC:
            if rx.search(nm):
                return n
        if NEUTRAL_CORLIB.search(nm):
            corlib = True
    if corlib:
        return "exec: AOT corlib (no IKVM/Java caller)"
    if js:
        return "exec: page JS (no wasm caller)"
    return "leaf: " + what_of(names[0]) if names else "leaf: ?"


def trigger_of(names):
    """Outermost anchor (closest to the thread root): which top-level activity this sample is part of."""
    for nm in reversed(names):
        for n, rx in ANCHORS:
            if rx.search(nm):
                return n
    return None


PKG = [("Mixin/ASM", rb"org\.objectweb\.asm|org\.spongepowered|org/objectweb|org_objectweb|org_spongepowered|com\.llamalad7|MixinExtras"),
       ("Fabric loader", rb"net\.fabricmc|net_fabricmc"), ("gson/JSON", rb"com\.google\.gson|com_google_gson"),
       ("guava", rb"com\.google\.common|com_google_common"), ("fastutil", rb"it\.unimi|it_unimi"),
       ("DFU/brigadier/authlib", rb"com\.mojang|com_mojang"), ("Minecraft", rb"net\.minecraft|net_minecraft"),
       ("mods (sodium/lithium/ranked/...)", rb"me\.jellysquid|com\.mcsrranked|me\.voidxwalker|com\.redlimerl|me\.contaria|"
                                            rb"com\.gregor0410|me\.jellysquid|net\.caffeinemc|krypton|xyz\.|dev\.|io\."),
       ("log4j", rb"org\.apache\.logging|org_apache_logging"), ("LWJGL", rb"org\.lwjgl|org_lwjgl"),
       ("JDK java.*/sun.*", rb"java\.|java_|sun\.|sun_|javax")]
PKG = [(n, re.compile(p)) for n, p in PKG]
JAVA_FRAME = re.compile(rb"^JS:[^ ]+ [^ ]+:[^ ]+ \(|^JS:IKVM_Java_")


def java_ctx(names):
    for nm in names:
        if JAVA_FRAME.search(nm):
            for n, rx in PKG:
                if rx.search(nm):
                    return n
            return "other Java"
    return "(no Java frame)"


# ---------------------------------------------------------------------------------------------------------------
def thread_tsv(d, t0, t1):
    """{tid: (comm, run_s, rq_s, wall_s, Counter(wchan when sleeping))} over [t0, t1] from threads.tsv."""
    p = os.path.join(d, "threads.tsv")
    if not os.path.exists(p):
        return {}
    first, last, wch = {}, {}, collections.defaultdict(collections.Counter)
    with open(p) as f:
        next(f)
        for line in f:
            t, pid, tid, comm, st, run, rq, wc = line.rstrip("\n").split("\t")
            t = float(t)
            if t < t0 or t > t1:
                continue
            tid = int(tid)
            if tid not in first:
                first[tid] = (t, int(run), int(rq), comm)
            last[tid] = (t, int(run), int(rq), comm)
            wch[tid]["R" if st == "R" else (wc if wc not in ("0", "-") else st)] += 1
    out = {}
    for tid, (ta, ra, qa, comm) in first.items():
        tb, rb, qb, comm = last[tid]
        out[tid] = (comm, (rb - ra) / 1e9, (qb - qa) / 1e9, tb - ta, wch[tid])
    return out


def open_taskclock(d):
    """perfraw.open_capture, keeping only task-clock samples when the capture also carries context-switch samples
    (capture 2 of R487). A fixed period is accepted only for task-clock, whose period is nanoseconds of CPU."""
    import mmap, hwcensus
    data = os.path.join(d, "perf.data")
    fh = open(data, "rb")
    mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
    at = [x for x in hwcensus.attrs_and_ids(mm) if x["stype"] & perfraw.S_CALLCHAIN]
    tc = [x for x in at if x["type"] == 1 and x["config"] == 1]
    if not tc:
        sys.exit("no task-clock attr: this reader needs time-proportional samples")
    phases = json.load(open(os.path.join(d, "phases.json")))
    if phases.get("period") and not tc:
        sys.exit("fixed-period capture of a non-time event")
    keep = set(i for x in tc for i in x["ids"]) if len(at) > 1 else None
    cap = perfraw.Capture(data, True, keep)
    maps = [os.path.join(d, f) for f in os.listdir(d) if f.startswith("perf-") and f.endswith(".map")]
    return cap, perfraw.Symbols(maps[0], cap.mmaps), phases


def main():
    a = sys.argv[1:]
    d = a[0]

    def opt(k, dflt=None):
        return a[a.index(k) + 1] if k in a else dflt
    cap, syms, phases = open_taskclock(d)
    hz = float(phases["freq"]) if not phases.get("period") else 1e9 / float(phases["period"])
    mk = marks_mono(d, phases)
    segs = [s for s in SEGMENTS + WORLD if s[1] in mk and s[2] in mk]
    # ROLES. The client main thread is the DedicatedWorker with the most samples between javaMain and titleReady.
    a0, a1 = int(mk["javaMain"] * 1e9), int(mk["titleReady"] * 1e9)
    cnt = collections.Counter(s[0] for s in cap.samples if a0 <= s[2] <= a1)
    pid = int(re.search(r"perf-(\d+)\.map", " ".join(os.listdir(d))).group(1))
    client = max((t for t in cnt if cap.comm.get(t, "").startswith("DedicatedWorker")), key=lambda t: cnt[t])

    def role(tid):
        c = cap.comm.get(tid, "?")
        if tid == client:
            return "client (Java main)"
        if tid == pid:
            return "browser main (JS)"
        if c.startswith("DedicatedWorker"):
            return "other pthreads"
        if c.startswith("ThreadPool"):
            return "V8/chromium pool"
        return "other chromium"
    ROLES = ["client (Java main)", "browser main (JS)", "other pthreads", "V8/chromium pool", "other chromium"]

    lk = syms.lookup
    seg_of = []
    for s in segs:
        seg_of.append((int(mk[s[1]] * 1e9), int(mk[s[2]] * 1e9), s[0]))
    tab = collections.defaultdict(collections.Counter)      # (seg, role) -> Counter(why/what key)
    symtab = collections.defaultdict(collections.Counter)   # class -> Counter(leaf sym)
    tot = collections.Counter()
    for tid, ip, t, chain in cap.samples:
        sname = None
        for b0, b1, n in seg_of:
            if b0 <= t <= b1:
                sname = n
                break
        if sname is None:
            continue
        r = role(tid)
        leaf = lk(ip) or b"[unknown]"
        names = [x for x in ((lk(c) for c in chain) if chain is not None else ()) if x]
        allf = [leaf] + names
        comp = component_of(allf)
        what = what_of(leaf)
        trig = trigger_of(allf) or "(none)"
        tab[(sname, r)][("why", comp)] += 1
        tab[(sname, r)][("what", what)] += 1
        tab[(sname, r)][("trig", trig)] += 1
        tab[(sname, r)][("java", java_ctx(allf))] += 1
        tier = re.search(rb"-(turbofan|liftoff)$", leaf)
        if tier and not JAVA_FRAME.search(leaf):
            tab[(sname, r)][("tier", tier.group(1).decode())] += 1
        tot[(sname, r)] += 1
        symtab[comp][leaf] += 1
        symtab["what:" + what][leaf] += 1
        if "--show" in a and opt("--show").startswith("chain:") and opt("--show")[6:].lower() in comp.lower() and len(symtab[comp]) and sum(symtab[comp].values()) % 997 == 1:
            print("--- chain sample:", comp); print("\n".join("   " + x.decode("latin1")[:120] for x in allf[:30]))

    if opt("--show"):
        want = opt("--show").lower()
        for k, c in symtab.items():
            if want in k.lower():
                n = sum(c.values())
                print(f"=== {k}: {n} samples ({n / hz:.1f} CPU s) ===")
                for s, v in c.most_common(int(opt("--top", "40"))):
                    print(f"  {v / hz:7.2f} s  {100 * v / n:5.1f}%  {s.decode('latin1')[:140]}")
        return

    print(f"{d}: {len(cap.samples)} samples at {hz:.0f} Hz; client thread tid {client}; renderer pid {pid}")
    print("\n=== fine segments (wall s), with per-role CPU s (task-clock) and off-CPU from threads.tsv ===")
    tt_all = {}
    for s in segs:
        t0, t1 = mk[s[1]], mk[s[2]]
        tt = thread_tsv(d, t0, t1)
        tt_all[s[0]] = tt
        rc = collections.Counter()
        for (sn, r), n in tot.items():
            if sn == s[0]:
                rc[r] += n
        ct = tt.get(client)
        cl = (f" | client run {ct[1]:5.1f} rq {ct[2]:4.1f} sleep {max(0, ct[3] - ct[1] - ct[2]):5.1f}" if ct else "")
        print(f"  {s[0]:<24} {t1 - t0:6.1f} s  " + "  ".join(f"{r.split(' ')[0]} {rc[r] / hz:5.1f}" for r in ROLES) + cl)
    want_kind = "what" if "--what" in a else "trig" if "--trig" in a else "java" if "--java" in a else "tier" if "--tier" in a else "why"
    roles = [opt("--role")] if opt("--role") else ROLES[:4]
    for r in roles:
        print(f"\n=== {r}: CPU seconds by {want_kind.upper()} per segment ===")
        keys = collections.Counter()
        for (sn, rr), c in tab.items():
            if rr == r:
                for (k, v), n in c.items():
                    if k == want_kind:
                        keys[v] += n
        cols = [s[0] for s in segs if not opt("--seg") or opt("--seg") in s[0]]
        print(f"  {'':<62}" + "".join(f"{c.split(' ')[0]:>7}" for c in cols) + "   total")
        for k, n in keys.most_common(int(opt("--top", "22"))):
            row = [tab[(c, r)][(want_kind, k)] / hz for c in cols]
            print(f"  {k[:62]:<62}" + "".join(f"{x:7.1f}" for x in row) + f"  {sum(row):6.1f}")
        row = [tot[(c, r)] / hz for c in cols]
        print(f"  {'TOTAL CPU s':<62}" + "".join(f"{x:7.1f}" for x in row) + f"  {sum(row):6.1f}")
    # off-CPU detail for the client
    print("\n=== client thread off-CPU: top wchan by segment (share of 100 ms samples) ===")
    for s in segs:
        ct = tt_all[s[0]].get(client)
        if ct:
            n = sum(ct[4].values())
            print(f"  {s[0]:<24} " + ", ".join(f"{k} {100 * v / n:.0f}%" for k, v in ct[4].most_common(4)))


if __name__ == "__main__":
    main()
