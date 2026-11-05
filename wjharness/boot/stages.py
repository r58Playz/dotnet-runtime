#!/usr/bin/env python3
"""stages.py <capdir> [STAGE [COMPONENT]] -- the client thread's boot CPU (segments 3-6) split by CLASS-DEFINITION STAGE.

Each sample goes to the INNERMOST stage frame in its chain (walking from the leaf). Interpreter frames are transparent
(their callee is unnamed, so the walk continues to the named caller). A named execution frame (JIT-emitted or AOT
managed/Java code that is not loader machinery) ends the walk: the sample is "executing code". Cross-tabbed with
bootattr's component, so a stage's own work can be told from JIT/interp/V8 tiering costs paid inside it.
"""
import collections, os, re, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib")); import paths  # noqa: E402,F401 -- every harness code dir on sys.path
import bootattr  # noqa: E402

STAGES = [
    ("SRE: CreateType (mono type bake)", rb"^JS:(corlib_System_Reflection_Emit_(RuntimeTypeBuilder_CreateType|TypeBuilder_CreateType)|"
                                          rb"ves_icall_TypeBuilder_create_runtime_class|mono_reflection_create_runtime_class)"),
    ("SRE: ILGenerator (writing IL)", rb"^JS:corlib_System_Reflection_Emit_(Runtime|Dynamic)?ILGenerator"),
    ("SRE: other (Define*, attributes, tokens)", rb"^JS:(corlib_System_Reflection_Emit_|mono_reflection_|mono_image_basic_init|mono_dynamic_image|"
                                                 rb"mono_image_create_|mono_image_build_metadata|ves_icall_.*(TypeBuilder|ModuleBuilder|"
                                                 rb"AssemblyBuilder|MethodBuilder|ILGenerator|DynamicMethod|RuntimeModuleBuilder))"),
    ("IKVM: verify/analyze bytecode", rb"^JS:IKVM_Runtime_IKVM_Runtime_(MethodAnalyzer|InstructionState|LocalVarInfo|UntangledExceptionTable|"
                                      rb"CodeInfo|ReturnAddressType|BranchInfo|ExceptionTableEntry|LocalVar)"),
    ("IKVM: bytecode -> IL (Compiler/CodeEmitter)", rb"^JS:IKVM_Runtime_IKVM_Runtime_(Compiler|CodeEmitter|Intrinsics|LambdaMetafactory|"
                                                    rb"AtomicReferenceFieldUpdaterEmitter|InterlockedMethods|BoxUtil)"),
    ("IKVM: Finish (defs, layout, other)", rb"^JS:IKVM_Runtime_IKVM_Runtime_(RuntimeByteCodeJavaType_(FinishContext|JavaTypeImpl|FinishedTypeImpl|"
                                           rb"Finish)|RuntimeJavaType_Finish)"),
    ("IKVM: ClassFile model (IKVM.Runtime parse)", rb"^JS:IKVM_Runtime_IKVM_Runtime_ClassFile"),
    ("IKVM.ByteCode: raw class-file decode", rb"^JS:(IKVM_ByteCode_|.*?\bIKVM\.ByteCode\.\S*:\S+ \()"),
    ("IKVM: bootstrap lookup", rb"findBootstrapClass|RuntimeAssemblyClassLoader_(LoadDynamic|FindDelegateResources|FindLoadedClass)"),
    ("IKVM: lazy-link binders (resolve)", rb"^JS:IKVM_Runtime_IKVM_Runtime_(ByteCodeHelper_Dynamic|DynamicBinder|.*TryCreateDirectBinder)"),
    ("Java static init (<clinit>)", rb"RuntimeJavaType_RunClassInit|:\.cctor \("),
    ("IKVM: define/link/load (other IKVM.Runtime)", rb"^JS:IKVM_Runtime_"),
    # Java-side loader machinery: checked on every frame before the execution test.
    ("Java: Mixin/ASM (transform + configs)", rb"org\.spongepowered|org\.objectweb\.asm|com\.llamalad7|org_objectweb|org_spongepowered"),
    ("Java: jar/zip read + inflate", rb"java\.util\.zip|java_util_zip|java\.util\.jar|java_util_jar|^JS:(inflate|adler32|crc32|zlib)"),
    ("Java: class-loader logic (Knot, ClassLoader)", rb"KnotClassLoader|net\.fabricmc\.loader\.impl\.launch|java_lang_ClassLoader_|"
                                                     rb"java\.lang\.ClassLoader:|URLClassLoader|java_net_URLClassLoader"),
]
STAGES = [(n, re.compile(p)) for n, p in STAGES]
EXEC = re.compile(rb"^JS:[^ ]+ [^ ]+:[^ ]+ \(|^JS:IKVM_Java_|^JS:wj_[a-z_]*stub|^JS:wj_lazy_bank|^JS:wasm-function\[")
INTERP = re.compile(rb"^JS:(mono_interp_exec_method|interp_exec_method)-")


def stage_of(names, want=None):
    for nm in names:
        for n, rx in STAGES:
            if rx.search(nm):
                return (n, nm) if want else n
        if INTERP.search(nm):
            continue
        if EXEC.search(nm):
            return ("executing code (not class definition)", nm) if want else "executing code (not class definition)"
    return ("no stage, no execution frame (runtime/V8/page)", b"") if want else "no stage, no execution frame (runtime/V8/page)"


def main():
    d = sys.argv[1]
    cap, syms, phases = bootattr.open_taskclock(d)
    hz = float(phases["freq"]) if not phases.get("period") else 1e9 / float(phases["period"])
    mk = bootattr.marks_mono(d, phases)
    segs = [s for s in bootattr.SEGMENTS[2:6] if s[1] in mk and s[2] in mk]
    a0, a1 = int(mk["javaMain"] * 1e9), int(mk["titleReady"] * 1e9)
    cnt = collections.Counter(s[0] for s in cap.samples if a0 <= s[2] <= a1)
    client = max((t for t in cnt if cap.comm.get(t, "").startswith("DedicatedWorker")), key=lambda t: cnt[t])
    bounds = [(int(mk[s[1]] * 1e9), int(mk[s[2]] * 1e9), s[0]) for s in segs]
    tab = collections.defaultdict(collections.Counter)   # stage -> seg -> n
    comp = collections.defaultdict(collections.Counter)  # stage -> bootattr component -> n
    tot = collections.Counter()
    drill = sys.argv[2] if len(sys.argv) > 2 else None
    dcomp = sys.argv[3] if len(sys.argv) > 3 else None
    frames = collections.Counter(); chains = []
    lk = syms.lookup
    for tid, ip, t, ch in cap.samples:
        if tid != client:
            continue
        sn = next((n for b0, b1, n in bounds if b0 <= t <= b1), None)
        if sn is None:
            continue
        names = [lk(ip) or b"[unknown]"] + [x for x in (lk(c) for c in (ch or ())) if x]
        st, fr = stage_of(names, True)
        if drill and drill.lower() in st.lower():
            c = bootattr.component_of(names)
            if not dcomp or dcomp.lower() in c.lower():
                frames[re.sub(rb"-\d+-(liftoff|turbofan)$", b"", fr)[:150]] += 1
                if len(chains) < 4 and sum(frames.values()) % 50 == 7:
                    chains.append(names[:40])
        tab[st][sn] += 1
        comp[st][bootattr.component_of(names)] += 1
        tot[sn] += 1
    T = sum(tot.values())
    if drill:
        n = sum(frames.values())
        print(f"=== {drill} / {dcomp}: {n / hz:.2f} CPU s; matched (innermost stage) frames:")
        for f, v in frames.most_common(25):
            print(f"  {v / hz:6.2f} s {100 * v / max(n, 1):5.1f}%  {f.decode('latin1')}")
        for ch in chains:
            print("--- chain:")
            print("\n".join("    " + x.decode("latin1")[:150] for x in ch))
        return
    cols = [s[0] for s in segs]
    print(f"{d}: client tid {client}, segments 3-6, {T / hz:.1f} CPU s profiled")
    print(f"  {'stage':<48}" + "".join(f"{c.split(' ')[0]:>7}" for c in cols) + "    CPU s   share")
    for st, _ in sorted(((k, sum(v.values())) for k, v in tab.items()), key=lambda x: -x[1]):
        row = [tab[st][c] / hz for c in cols]
        print(f"  {st[:48]:<48}" + "".join(f"{x:7.2f}" for x in row) + f"  {sum(row):7.2f}  {100 * sum(row) * hz / T:5.1f}%")
    print("  " + "-" * 48 + "".join(f"{tot[c] / hz:7.2f}" for c in cols))
    print("\n  inside each stage, by bootattr component (% of the stage):")
    for st, _ in sorted(((k, sum(v.values())) for k, v in tab.items()), key=lambda x: -x[1]):
        n = sum(comp[st].values())
        if n / T < 0.02:
            continue
        top = ", ".join(f"{c.split(' (')[0]} {100 * v / n:.0f}%" for c, v in comp[st].most_common(5))
        print(f"  {st[:48]:<48} {top}")


if __name__ == "__main__":
    main()
