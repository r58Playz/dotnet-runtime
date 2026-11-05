// Turn a perf symbol into (provenance, subsystem) for this app's stack.
//
// Extracted from mcreport.mjs so it can be validated against a real corpus rather than trusted. The
// classifier is the whole value of that report -- a bucket that swallows 40% of the profile as
// "unclassified" is worse than no bucketing, because it looks like an answer. `node lib/symclass.mjs
// <file-with-perf-report-output>` prints the distribution and the largest unclassified names.
//
// V8 names every generated frame `JS:<name>-<scriptId>-<tier>`, and two facts about this app fall out of
// that which make provenance readable without dumping modules the way pattern.mjs --ours does:
//
//   * dotnet.native.wasm's functions carry a real scriptId (six digits here) and are spelled in the AOT/C
//     mangling: mono_interp_exec_method, IKVM_Runtime_..._FromClass_java_lang_Class.
//   * OUR generated modules are instantiated from raw bytes with no script, so V8 assigns scriptId 0, and
//     MONO_WASM_JIT_NAMES writes mono's own full-name spelling into their name section:
//     `bool net.minecraft.server.MinecraftServer:method_20415 ()`.
//
// Both are heuristics over a naming convention, so `ours` (an exact emitted-name Set from
// lib/wasmnames.mjs) overrides them whenever a caller has one.

/** "JS:<name>-<scriptId>-<tier>" -> { name, scriptId, tier, js }. */
export function parseSym (sym) {
  // Newer perf appends IPC + coverage columns ("  -  -") after the symbol.
  const s = String(sym).replace(/\s+-\s+-\s*$/, "").trim();
  // Anchor on the END: the name itself legally contains '-' (wasm-to-js, jit-call...), so splitting on
  // '-' truncates names. V8 writes `-none` for some wrappers and no suffix at all for others.
  const m = s.match(/^JS:(.*)-(\d+)-(turbofan|liftoff|interpreter|none|builtin)$/);
  if (m) return { name: m[1], scriptId: Number(m[2]), tier: m[3], js: true };
  const m2 = s.match(/^JS:(.*)-(turbofan|liftoff|interpreter|none|builtin)$/);
  if (m2) return { name: m2[1], scriptId: null, tier: m2[2], js: true };
  if (s.startsWith("JS:")) return { name: s.slice(3), scriptId: null, tier: null, js: true };
  return { name: s, scriptId: null, tier: null, js: false };
}

/* Mono's full-name spelling -- `<ret> <Type>:<method> (<args>)`, or a `(wrapper ...)` -- as opposed to the
 * AOT/C mangling, which has neither spaces nor colons. This is what MONO_WASM_JIT_NAMES emits. */
export const isMonoFullName = (n) => /^\(wrapper /.test(n) || (/:\S/.test(n) && /\s/.test(n));
const isHex = (n) => /^0x[0-9a-f]+$/i.test(n);

export function provenance (p, ours = null) {
  if (!p.js) {
    // A bare hex address with no JS: prefix is an unresolved mapping, not native code.
    if (isHex(p.name)) return "UNSYMBOLISED";
    return "native (libc / vdso / chromium)";
  }
  if (isHex(p.name)) return "UNSYMBOLISED";
  if (/^Builtin:/.test(p.name)) return "V8 builtin";
  if (/^(wasm-to-js|js-to-wasm|GenericJSToWasmWrapper|wasm_to_js)/.test(p.name)) return "V8 wasm<->js wrapper";
  // A URL means real page JavaScript (the loader, log.ts, the vite client), not generated code.
  if (/https?:\/\//.test(p.name)) return "page JavaScript";
  if (ours) return ours.has(p.name) ? "our wasm JIT" : "AOT image (dotnet.native.wasm)";
  /* NO scriptId CONSTRAINT. This used to require `p.scriptId === 0`, and that single clause produced a
   * materially wrong headline: it reported the wasm-JIT tier as 2.80% of the in-game window when the true
   * figure is ~55%, because V8 stamps these functions with whatever script id it likes (0..7 observed in one
   * capture) and only the id-0 slice was ever credited. Everything else fell through to "AOT image", so the
   * tier's own method bodies were attributed to the AOT runtime and the conclusion "no mono-side change can
   * exceed 2.8%" followed from a parsing detail.
   *
   * The naming convention is the whole discriminator and needs no help from the id: mono full names
   * (`<ret> <Type>:<method> (<args>)`, and `(wrapper ...)`) are what MONO_WASM_JIT_NAMES writes into each
   * JITted module's name section, whereas AOT methods in dotnet.native.wasm carry underscore-mangled C
   * symbols with neither spaces nor colons (`IKVM_Java_java_util_HashMap_getNode_int_object`). A DynamicMethod
   * -- `(wrapper dynamic-method) ...` -- cannot be AOT code at all, since it is generated at runtime. */
  if (isMonoFullName(p.name)) return "our wasm JIT";
  if (/^(jiterp|trace_|do_jit_call_)/.test(p.name)) return "jiterpreter trace";
  return "AOT image (dotnet.native.wasm)";
}

/*
 * Subsystem, in priority order -- and the order is load-bearing twice over: `mono_wasm_jit_*` must be
 * tested before the generic `mono_*` fallback, and the Java-package tests must come before the
 * mono-full-name catch-all or every JIT'd Minecraft method lands in "managed (other)".
 *
 * The selection criterion for a bucket is that it names something that could actually be worked on. "GC"
 * and "interpreter" are separate because the fixes are unrelated. "wasm-JIT compile/admit" and "wasm-JIT
 * dispatch" are separate because one is a startup cost and the other a steady-state tax, and conflating
 * them is how you conclude the JIT is expensive without learning which half to attack.
 */
export const SUBSYSTEMS = [
  // --- our runtime work ---------------------------------------------------------------------------
  /* Three spellings of the same tier, all present in the image: `wj_*` (internal helpers),
   * `mono_wasm_jit_*` (the exported surface) and a bare `wasm_jit_*` (wasm_jit_maybe_compile,
   * wasm_jit_prepare_interp_callee). Missing the third routed real dispatch work into "unclassified". */
  [/^(wj_auto_batch|wj_plan|wj_emit|wj_compile|wj_island|wj_batch|wj_module|wj_park|wj_promote|wj_queue|wj_scc|wj_hit|wj_admit|wj_reserve|(mono_)?wasm_jit_(compile|emit|generate|admit|register|island|promote|batch|reserve|plan|hit|queue|snapshot|maybe_compile))/, "wasm-JIT compile/admit"],
  [/^(wj_(delegate|vcall|ic_|fslot|residual|thunk|entry|invoke|sync)|(mono_)?wasm_jit_(vcall|call_interp|invoke|entry|residual|delegate|ic_|profile|sync|thunk|fslot|prepare_interp))/, "wasm-JIT dispatch (vcall/residual/entry)"],
  [/^(wj_|(mono_)?wasm_jit_)/, "wasm-JIT other"],
  [/^(mono_jiterp_|jiterp_|jiterpreter)/, "jiterpreter support"],
  // --- mono runtime ------------------------------------------------------------------------------
  /* get_virtual_method_fast / get_arg_offset_fast / lock_thread / jit_info_table_find are interp.c and
   * mini/ statics, so perf shows them unprefixed -- they look like anonymous C helpers and fell through. */
  [/^(mono_interp_|interp_|ves_exec|do_jit_call|jit_call_cb|mono_mint_type|stackval|mint_|get_context|get_virtual_method|lock_thread|jit_info_table)/, "interpreter"],
  [/^(sgen_|major_|minor_|nursery_|mono_gc_|mono_sgen|GC_|copy_object|scan_object|mono_gchandle|mono_handle_new|mono_handle_stack|par_copy|alloc_obj|mono_object_new|aot_wrapper_alloc|.*_AllocSmall)/, "GC (sgen)"],
  [/^(mono_class_|mono_type_|mono_metadata_|mono_image_|mono_assembly_|mono_domain_|mono_field_|mono_method_|mono_signature_|mono_marshal_|mono_custom_attr|mono_internal_hash_table|mono_generic_|mono_aot_|mono_ldtoken|mono_defaults)/, "mono type/metadata system"],
  [/^(mono_reflection_|ves_icall_RuntimeType|ves_icall_System_Reflection|mono_runtime_invoke|mono_property_)/, "reflection / runtime-invoke"],
  [/(catch_exception|mono_handle_exception|mono_unwind|mono_exception|throw_exception|_Unwind|__cxa_|mono_llvm_(raise|throw|match|clear)|mono_wasm_(raise|eh))/, "exception handling / unwind"],
  [/^(mono_threads_|mono_thread_|mono_coop|mono_monitor|mono_os_|mono_atomic|mono_lifo|mono_native_thread)/, "mono threading/state transitions"],
  /* gsharedvt BEFORE the corlib/assembly prefixes below: the biggest single fall-through in the previous
   * session's capture was `corlib_aot_wrapper_gsharedvt_out_sig_pinvoke_obj_obj` at ~1.3%, which is not
   * "some corlib method" -- it is a generic-sharing marshalling wrapper, i.e. a distinct and separately
   * fixable cost (a method that could not be shared concretely). Merging it into corlib hides that. */
  [/gsharedvt|gshared_/, "gsharedvt wrappers (generic sharing)"],
  [/^ves_icall/, "internal calls (icall)"],
  /* mini/ is mono's shared JIT+interp infrastructure and eglib is its glib shim; both showed up only as
   * fall-through (mini_type_get_underlying_type, get_arg_offset_fast, monoeg_g_array_set_size). */
  [/^(mini_|get_arg_offset|do_mono_metadata|mono_metadata)/, "mono type/metadata system"],
  [/^monoeg_/, "eglib (glib shim)"],
  [/^mono_/, "mono runtime (other)"],
  // --- IKVM --------------------------------------------------------------------------------------
  [/(RuntimeJavaType|RuntimeByteCodeJavaType|RuntimeJavaMethod|RuntimeJavaField|_TypeAsTBD|ClassLoaderWrapper|ByteCodeHelper_(Cast|DynamicCast|InstanceOf|DynamicGetfield|DynamicPutfield|Dynamic))/, "IKVM type system"],
  /* PairwiseConvert is in here on purpose. `(wrapper dynamic-method) int object:PairwiseConvert (...)` was
   * the single largest MANAGED fall-through in the previous capture (~3% across its variants) and reads
   * like an unrelated conversion helper; it is in fact IKVM's LambdaForm argument-adapter machinery, so
   * counting it anywhere but here understates MethodHandle dispatch by a third. */
  [/(DynamicMethodBuilder|MethodHandleUtil|DynamicBinder|DMH\.|LambdaForm|MethodHandle|invokeBasic|invokeExact|InvokeExact|checkReceiver|BoundMethodHandle|DirectMethodHandle|invokeInterface_|invokeVirtual_|invokeStatic_|invokeSpecial_|PairwiseConvert|linkToTargetMethod|ValueConversions|\bBMH\.|\bMH\.)/, "MethodHandle / LambdaForm dispatch"],
  [/sun_misc_Unsafe|sun\.misc\.Unsafe/, "sun.misc.Unsafe externs"],
  [/IkvmClassLoader|IkvmWasm_ClassLoader|ClassLoader_LoadClass|_defineClass|_findClass/, "IKVM class loading"],
  [/^IKVM_Runtime_/, "IKVM runtime (other)"],
  [/^IKVM_Java_/, "IKVM java.* implementation"],
  // --- Java application --------------------------------------------------------------------------
  [/net\.minecraft\.|net_minecraft_/, "Minecraft code"],
  [/me\.jellysquid|\bca\.\w+\.|com\.mojang|net\.fabricmc|org\.spongepowered|nl\.jellyjolt|com\.redlimerl/, "mods / Fabric / Mojang libs"],
  [/it\.unimi\.dsi\.fastutil|^ikvmc_fastutil/, "fastutil collections"],
  [/\b(java|javax)\.(lang|util|io|nio|net|security|text|math|time|zip)\b|org\.apache\.commons/, "java.* library"],
  /* AOT'd managed code, named by the assembly it came from. mono's AOT mangling is
   * `<assembly>_<Namespace>_<Type>_<Method>_<argtypes>`, so the prefix is the only thing that says which
   * assembly a symbol belongs to -- and without these the whole AOT image reads as "unclassified".
   * Deliberately AFTER the semantic buckets above, so a corlib Dictionary lookup still lands in corlib but
   * an ikvmc_lwjgl3 MethodHandle adapter lands in MethodHandle dispatch. */
  [/^ikvmc_lwjgl3_|org\.lwjgl/, "LWJGL bindings (AOT)"],
  [/^ikvmc_(guava|gson|netty|log4j|asm|jvmdowngrader)/, "bundled jar (AOT: guava/gson/netty/log4j/asm)"],
  [/^ikvmc_/, "ikvmc-compiled jar (other, AOT)"],
  [/^corlib_/, "corlib / BCL (AOT)"],
  [/^(System_|Microsoft_)/, "BCL assembly (AOT)"],
  // --- platform ----------------------------------------------------------------------------------
  [/^(dlmalloc|dlfree|dlrealloc|dlcalloc|dlmallinfo|dlposix|emscripten_builtin_|sbrk|malloc|free|realloc|calloc|memcpy|memset|memmove|memcmp|strlen|strcmp|try_realloc|mmap|munmap)/, "allocator / memcpy"],
  [/^(pthread_|__pthread|emscripten_futex|futex|__wait|timedwait|__timedwait|sem_|__lock|__unlock|_emscripten_thread|emscripten_yield|emscripten_check_mailbox|emscripten_main_thread)/, "threading primitives / futex"],
  [/^(_?_?emscripten|wasmfs|WasmFS|__syscall|opfs|OPFS|_?fd_|__stdio|fwrite|printf|vfprintf|puts)/, "emscripten / stdio / WASMFS"],
  [/^(glfw|GLFW|webgl|WebGL|gl[A-Z]|_?emscripten_glfw|Krypton|krypton)/, "GL / GLFW"],
  /* OpenAL's C++ symbols are demangled by perf, so they look nothing like the al* C API:
   * `DeviceBase::renderSamples(unsigned int)`, `ResampleFrame_Stereo`, `MixHrtf*`. */
  [/^(al[A-Z]|AL_|openal|OpenAL|alc[A-Z])|^(DeviceBase|ALCdevice|ALCcontext|ALsource|Resample|MixHrtf|MixDirect|ALvoice)|::(mixVoice|renderSamples|process)/, "OpenAL"],
  [/^(inflate|deflate|zlib|crc32|adler32|png_|stbi_|libpng|_?tinfl|LZ4)/, "decompress / image decode"],
  [/^(expf?|logf?|powf?|sinf?|cosf?|tanf?|sqrtf?|atan2?f?|fmodf?|ceilf?|floorf?|__math)/, "libm"],
  [/^Builtin:/, "V8 builtins"],
  [/^(wasm-to-js|js-to-wasm|GenericJSToWasmWrapper)/, "V8 wasm<->js wrappers"],
  [/^(__vdso|clock_gettime|__clock|gettimeofday)/, "clock / vdso"],
  [/https?:\/\//, "page JavaScript (loader / log.ts / vite)"],
];

export function subsystem (p) {
  if (isHex(p.name)) return "UNSYMBOLISED";
  for (const [rx, label] of SUBSYSTEMS) if (rx.test(p.name)) return label;
  if (isMonoFullName(p.name)) return "managed (other)";
  return "other / unclassified";
}

/* --- self-check ---------------------------------------------------------------------------------- *
 * node lib/symclass.mjs <perf-report-stdio-output>
 * Prints the bucket distribution and the biggest names that fell through, which is the only way to tell a
 * classifier that WORKS from one that quietly routes half the profile into a catch-all.
 */
if (process.argv[1] && process.argv[1].endsWith("symclass.mjs")) {
  const { readFileSync } = await import("fs");
  const file = process.argv[2];
  if (!file) { console.error("usage: symclass.mjs <file containing `perf report --stdio` output>"); process.exit(1); }
  const rows = [];
  for (const line of readFileSync(file, "utf8").split("\n")) {
    const m = line.match(/^\s*(\d+\.\d+)%\s+\[[.k]\]\s+(.+?)\s*$/);
    if (m) rows.push({ pct: parseFloat(m[1]), sym: m[2] });
  }
  console.log(`${rows.length} rows, ${rows.reduce((a, b) => a + b.pct, 0).toFixed(1)}% total`);
  const bySub = new Map(), byProv = new Map(), fell = [];
  for (const r of rows) {
    const p = parseSym(r.sym);
    const s = subsystem(p), v = provenance(p);
    bySub.set(s, (bySub.get(s) || 0) + r.pct);
    byProv.set(v, (byProv.get(v) || 0) + r.pct);
    if (s === "other / unclassified" || s === "managed (other)") fell.push({ ...r, s, name: p.name });
  }
  for (const [label, m] of [["provenance", byProv], ["subsystem", bySub]]) {
    console.log(`\n=== by ${label} ===`);
    for (const [k, v] of [...m.entries()].sort((a, b) => b[1] - a[1]))
      console.log(`  ${v.toFixed(2).padStart(7)}%  ${k}`);
  }
  console.log(`\n=== biggest names that fell through (${fell.length} rows, ${fell.reduce((a, b) => a + b.pct, 0).toFixed(1)}%) ===`);
  for (const f of fell.sort((a, b) => b.pct - a.pct).slice(0, 30))
    console.log(`  ${f.pct.toFixed(2).padStart(6)}%  [${f.s}] ${f.name.slice(0, 110)}`);
}
