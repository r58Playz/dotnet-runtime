// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

import { MonoMethod, MonoType, PThreadPtrNull } from "./types/internal";
import { NativePointer, VoidPtr } from "./types/emscripten";
import { mono_assert } from "./globals";
import {
    getU32_unaligned,
    free, malloc, localHeapViewU8
} from "./memory";
import { WasmOpcode } from "./jiterpreter-opcodes";
import cwraps from "./cwraps";
import {
    WasmBuilder, addWasmFunctionPointer, getMemberOffset,
    _now, getRawCwrap, importDef,
    getWasmFunctionTable, recordFailure, getOptions,
    JiterpreterOptions,
    getCounter, modifyCounter
} from "./jiterpreter-support";
import { WasmValtype } from "./jiterpreter-opcodes";
import { mono_log_error, mono_log_info } from "./logging";
import { utf8ToString } from "./strings";
import WasmEnableThreads from "consts:wasmEnableThreads";
import { mono_wasm_pthread_ptr } from "./pthreads/shared";
import {
    JiterpreterTable, JiterpCounter, JitQueue, JiterpMember
} from "./jiterpreter-enums";

// Controls miscellaneous diagnostic output.
const trace = 0;
const
    // Dumps all compiled wrappers
    dumpWrappers = false;

/*
typedef struct {
    InterpMethod *rmethod;
    gpointer this_arg;
    gpointer res;
    gpointer args [16];
    gpointer *many_args;
} InterpEntryData;

typedef struct {
    InterpMethod *rmethod; // 0
    ThreadContext *context; // 4
    gpointer orig_domain; // 8
    gpointer attach_cookie; // 12
    int params_count; // 16
} JiterpEntryDataHeader;
*/

const
    maxInlineArgs = 16;

// Scratch for mono_wasm_jit_entry_sig's two output arrays; allocated once, never freed.
let wjSigScratch: VoidPtr | undefined;

const maxJitQueueLength = 4,
    queueFlushDelayMs = 10;

let trampBuilder: WasmBuilder;
let trampImports: Array<[string, string, Function]> | undefined;
let fnTable: WebAssembly.Table;
let jitQueueTimeout = 0;
const infoTable: { [ptr: number]: TrampolineInfo } = {};

/*
const enum WasmReftype {
    funcref = 0x70,
    externref = 0x6F,
}
*/

function getTrampImports () {
    if (trampImports)
        return trampImports;

    trampImports = [
        importDef("interp_entry_prologue", getRawCwrap("mono_jiterp_interp_entry_prologue")),
        importDef("interp_entry", getRawCwrap("mono_jiterp_interp_entry")),
        importDef("unbox", getRawCwrap("mono_jiterp_object_unbox")),
        importDef("stackval_from_data", getRawCwrap("mono_jiterp_stackval_from_data")),
        importDef("wasm_jit_admitted", getRawCwrap("mono_jiterp_wasm_jit_admitted")),
    ];

    return trampImports;
}

class TrampolineInfo {
    imethod: number;
    method: MonoMethod;
    paramTypes: Array<NativePointer>;

    argumentCount: number;
    hasThisReference: boolean;
    unbox: boolean;
    hasReturnValue: boolean;
    private name?: string;
    private traceName?: string;

    defaultImplementation: number;
    result: number;
    hitCount: number;
    guardedImplementation?: Function;
    directImplementation?: Function;
    directInstalled = false;

    // wasm-JIT direct-forward info. wjNargs >= 0 means this signature can be forwarded straight to the
    // JITted body's f-slot: wjKinds[i] is how to load argument i from its pointer, wjVtypes[i] its wasm
    // type, and wjVtypes[wjNargs] the return type. Computed in C from the same call-info classification
    // the emitter used to build `f`, because wasm checks call_indirect signatures exactly.
    wjNargs = -1;
    wjKinds: Array<number> = [];
    wjVtypes: Array<number> = [];

    constructor (
        imethod: number, method: MonoMethod, argumentCount: number, pParamTypes: NativePointer,
        unbox: boolean, hasThisReference: boolean, hasReturnValue: boolean, defaultImplementation: number
    ) {
        this.imethod = imethod;
        this.method = method;
        this.argumentCount = argumentCount;
        this.unbox = unbox;
        this.hasThisReference = hasThisReference;
        this.hasReturnValue = hasReturnValue;
        this.paramTypes = new Array(argumentCount);
        for (let i = 0; i < argumentCount; i++)
            this.paramTypes[i] = <any>getU32_unaligned(<any>pParamTypes + (i * 4));
        this.defaultImplementation = defaultImplementation;
        this.result = 0;
        this.hitCount = 0;
        this.computeWasmJitSig();
    }

    private computeWasmJitSig () {
        // is_invoke / needs_thread_attach are per-method constants the fast path cannot honour; check
        // them once here so the generated code carries no test for them.
        if (!cwraps.mono_jiterp_wasm_jit_entry_ok(this.imethod))
            return;
        if (this.unbox)
            return;
        if (!wjSigScratch)
            wjSigScratch = malloc(2 * (maxInlineArgs + 2));
        const kinds = <any>wjSigScratch as number;
        const vtypes = kinds + (maxInlineArgs + 2);
        const n = cwraps.mono_wasm_jit_entry_sig(this.method, kinds, vtypes, maxInlineArgs + 1);
        if (n < 0 || n !== this.argumentCount + (this.hasThisReference ? 1 : 0))
            return;
        const heap = localHeapViewU8();
        for (let i = 0; i < n; i++) {
            this.wjKinds.push(heap[kinds + i]);
            this.wjVtypes.push(heap[vtypes + i]);
        }
        this.wjVtypes.push(heap[vtypes + n]); // return valtype (0x40 = void)
        this.wjNargs = n;
    }

    generateName () {
        const namePtr = cwraps.mono_wasm_method_get_full_name(this.method);
        try {
            const name = utf8ToString(namePtr);
            this.name = name;
            let subName = name;
            if (!subName) {
                subName = `${this.imethod.toString(16)}_${this.hasThisReference ? "i" : "s"}${this.hasReturnValue ? "_r" : ""}_${this.argumentCount}`;
            } else {
                // truncate the real method name so that it doesn't make the module too big. this isn't a big deal for module-per-function,
                //  but since we jit in groups now we need to keep the sizes reasonable. we keep the tail end of the name
                //  since it is likely to contain the method name and/or signature instead of type and noise
                const maxLength = 24;
                if (subName.length > maxLength)
                    subName = subName.substring(subName.length - maxLength, subName.length);
                subName = `${this.imethod.toString(16)}_${subName}`;
            }
            this.traceName = subName;
        } finally {
            if (namePtr)
                free(<any>namePtr);
        }
    }

    getTraceName () {
        if (!this.traceName)
            this.generateName();
        return this.traceName || "unknown";
    }

    getName () {
        if (!this.name)
            this.generateName();
        return this.name || "unknown";
    }
}

let mostRecentOptions: JiterpreterOptions | undefined = undefined;

function has_live_pthread () {
    return !WasmEnableThreads || mono_wasm_pthread_ptr() !== PThreadPtrNull;
}

// If a method is freed we need to remove its info (just in case another one gets
//  allocated at that exact memory offset later) and more importantly, ensure it is
//  not waiting in the jit queue
export function mono_jiterp_free_method_data_interp_entry (imethod: number) {
    // Normalize so an imethod pointer >= 2GB matches the unsigned key used by infoTable
    imethod = imethod >>> 0;
    const info = infoTable[imethod];
    // A direct adapter is installed only in this worker's table. Restore the signature-correct slow
    // implementation before dropping the JS references so a stale AOT vtable entry can never retain
    // or enter a freed adapter module.
    if (info && fnTable && info.result > 0) {
        const fallback = fnTable.get(info.defaultImplementation);
        if (fallback)
            fnTable.set(info.result, fallback);
    }
    delete infoTable[imethod];
}

// Called by mono_wasm_jit_admit after the method's complete direct-call closure has been installed in
// THIS worker's function table. Every worker owns a distinct table and a distinct infoTable, so this
// patches only the worker whose admission just succeeded.
export function mono_jiterp_wasm_jit_patch_interp_entry (imethod: number) {
    imethod = imethod >>> 0;
    const info = infoTable[imethod];
    if (!info || !info.directImplementation || info.directInstalled || !fnTable || info.result <= 0)
        return;
    fnTable.set(info.result, info.directImplementation);
    info.directInstalled = true;
}

// Automatic rebatching reuses the target's f-slot. Admission calls this before replacing a worker's
// slot so an already-installed guard-free adapter cannot enter the new generation prematurely.
export function mono_jiterp_wasm_jit_unpatch_interp_entry (imethod: number) {
    imethod = imethod >>> 0;
    const info = infoTable[imethod];
    if (!info || !info.directInstalled || !fnTable || info.result <= 0)
        return;
    const guarded = info.guardedImplementation || fnTable.get(info.defaultImplementation);
    if (guarded)
        fnTable.set(info.result, guarded);
    info.directInstalled = false;
}

// FIXME: move this counter into C and make it thread safe
export function mono_interp_record_interp_entry (imethod: number) {
    // clear the unbox bit
    imethod = (imethod & ~0x1) >>> 0;

    const info = infoTable[imethod];
    // This shouldn't happen but it's not worth crashing over
    if (!info)
        return;

    if (!mostRecentOptions)
        mostRecentOptions = getOptions();

    info.hitCount++;
    if (info.hitCount === mostRecentOptions!.interpEntryFlushThreshold)
        flush_wasm_entry_trampoline_jit_queue();
    else if (info.hitCount !== mostRecentOptions!.interpEntryHitCount)
        return;

    if (!has_live_pthread())
        return;

    const jitQueueLength = cwraps.mono_jiterp_tlqueue_add(JitQueue.InterpEntry, <any>imethod);
    if (jitQueueLength >= maxJitQueueLength)
        flush_wasm_entry_trampoline_jit_queue();
    else
        ensure_jit_is_scheduled();
}

// returns function pointer
export function mono_interp_jit_wasm_entry_trampoline (
    imethod: number, method: MonoMethod, argumentCount: number, pParamTypes: NativePointer,
    unbox: boolean, hasThisReference: boolean, hasReturnValue: boolean, defaultImplementation: number
): number {
    // HACK
    if (argumentCount > maxInlineArgs)
        return 0;

    imethod = imethod >>> 0;
    method = method as any >>> 0 as any;
    pParamTypes = pParamTypes as any >>> 0 as any;

    const info = new TrampolineInfo(
        imethod, method, argumentCount, pParamTypes,
        unbox, hasThisReference, hasReturnValue, defaultImplementation
    );
    if (!fnTable)
        fnTable = getWasmFunctionTable();

    // We start by creating a function pointer for this interp_entry trampoline, but instead of
    //  compiling it right away, we make it point to the default implementation for that signature
    // This gives us time to wait before jitting it so we can jit multiple trampolines at once.
    // Some entry wrappers are also only called a few dozen times, so it's valuable to wait
    //  until a wrapper is called a lot before wasting time/memory jitting it.
    const defaultImplementationFn = fnTable.get(defaultImplementation);
    const tableId = (hasThisReference
        ? (
            hasReturnValue
                ? JiterpreterTable.InterpEntryInstanceRet0
                : JiterpreterTable.InterpEntryInstance0
        )
        : (
            hasReturnValue
                ? JiterpreterTable.InterpEntryStaticRet0
                : JiterpreterTable.InterpEntryStatic0
        )) + argumentCount;
    info.result = addWasmFunctionPointer(tableId, defaultImplementationFn);

    infoTable[imethod] = info;
    return info.result;
}

function ensure_jit_is_scheduled () {
    if (jitQueueTimeout > 0)
        return;

    if (typeof (globalThis.setTimeout) !== "function")
        return;

    // We only want to wait a short period of time before jitting the trampolines.
    // In practice the queue should fill up pretty fast during startup, and we just
    //  want to make sure we catch the last few stragglers with this timeout handler.
    // Note that in console JS runtimes this means we will never automatically flush
    //  the queue unless it fills up, which is unfortunate but not fixable since
    //  there is no realistic way to efficiently maintain a hit counter for these trampolines
    jitQueueTimeout = globalThis.setTimeout(() => {
        jitQueueTimeout = 0;
        if (!has_live_pthread())
            return;
        flush_wasm_entry_trampoline_jit_queue();
    }, queueFlushDelayMs);
}

function flush_wasm_entry_trampoline_jit_queue () {
    if (!has_live_pthread())
        return;

    const jitQueue : TrampolineInfo[] = [];
    let methodPtr = <MonoMethod><any>0;
    // tlqueue_next returns an i32, so an imethod pointer >= 2GB comes back as a negative
    //  number. infoTable is keyed by the unsigned value, so normalize before lookup.
    while ((methodPtr = <any>(cwraps.mono_jiterp_tlqueue_next(JitQueue.InterpEntry) as any >>> 0)) != 0) {
        const info = infoTable[<any>methodPtr];
        if (!info) {
            mono_log_info(`Failed to find corresponding info for method ptr ${methodPtr} from jit queue!`);
            continue;
        }
        jitQueue.push(info);
    }

    if (!jitQueue.length)
        return;

    // If the function signature contains types that need stackval_from_data, that'll use
    //  some constant slots, so make some extra space
    let builder = trampBuilder;
    if (!builder) {
        trampBuilder = builder = new WasmBuilder();

        builder.defineType(
            "unbox",
            {
                "pMonoObject": WasmValtype.i32,
            },
            WasmValtype.i32, true
        );
        builder.defineType(
            "interp_entry_prologue",
            {
                "rmethod": WasmValtype.i32,
                "this_arg": WasmValtype.i32,
                "params_count": WasmValtype.i32,
            },
            WasmValtype.i32, true
        );
        builder.defineType(
            "interp_entry",
            {
                "res": WasmValtype.i32,
            },
            WasmValtype.void, true
        );
        builder.defineType(
            "stackval_from_data",
            {
                "type": WasmValtype.i32,
                "result": WasmValtype.i32,
                "value": WasmValtype.i32
            },
            WasmValtype.void, true
        );
    } else
        builder.clear();

    if (builder.options.wasmBytesLimit <= getCounter(JiterpCounter.BytesGenerated)) {
        return;
    }

    const started = _now();
    let compileStarted = 0;
    let rejected = true, threw = false;

    try {
        // Magic number and version
        builder.appendU32(0x6d736100);
        builder.appendU32(1);

        for (let i = 0; i < jitQueue.length; i++) {
            const info = jitQueue[i];

            const sig: any = {};
            if (info.hasThisReference)
                sig["this_arg"] = WasmValtype.i32;
            if (info.hasReturnValue)
                sig["res"] = WasmValtype.i32;
            for (let i = 0; i < info.argumentCount; i++)
                sig[`arg${i}`] = WasmValtype.i32;
            sig["rmethod"] = WasmValtype.i32;

            // Function type for compiled traces
            builder.defineType(
                info.getTraceName(), sig, WasmValtype.void, false
            );

            // ...and the JITted method's own signature, for the direct call_indirect fast path.
            if (info.wjNargs >= 0) {
                const tsig: any = {};
                for (let a = 0; a < info.wjNargs; a++)
                    tsig[`p${a}`] = info.wjVtypes[a];
                builder.defineType(
                    info.getTraceName() + "_wj", tsig, info.wjVtypes[info.wjNargs], false
                );
            }
        }

        builder.generateTypeSection();

        // Import section
        const trampImports = getTrampImports();
        builder.compressImportNames = true;

        // Emit function imports
        for (let i = 0; i < trampImports.length; i++) {
            mono_assert(trampImports[i], () => `trace #${i} missing`);
            builder.defineImportedFunction("i", trampImports[i][0], trampImports[i][1], true, trampImports[i][2]);
        }

        // Assign import indices so they get emitted in the import section
        for (let i = 0; i < trampImports.length; i++)
            builder.markImportAsUsed(trampImports[i][0]);

        // Import the indirect function table: the wasm-JIT direct-forward path call_indirects a
        // JITted method's f-slot in it.
        builder._generateImportSection(true);

        const directQueue = jitQueue.filter((info) => info.wjNargs >= 0);

        // Function section
        builder.beginSection(3);
        builder.appendULeb(jitQueue.length + directQueue.length);
        for (let i = 0; i < jitQueue.length; i++) {
            const info = jitQueue[i];
            const traceName = info.getTraceName();
            // Function type for our compiled trace
            mono_assert(builder.functionTypes[traceName], "func type missing");
            builder.appendULeb(builder.functionTypes[traceName][0]);
        }
        // A direct adapter has the same native/AOT-facing pointer ABI as its guarded wrapper.
        for (let i = 0; i < directQueue.length; i++) {
            const traceName = directQueue[i].getTraceName();
            builder.appendULeb(builder.functionTypes[traceName][0]);
        }

        // Export section
        builder.beginSection(7);
        builder.appendULeb(jitQueue.length + directQueue.length);
        for (let i = 0; i < jitQueue.length; i++) {
            const info = jitQueue[i];
            const traceName = info.getTraceName();
            builder.appendName(traceName);
            builder.appendU8(0);
            // Imports get added to the function index space, so we need to add
            //  the count of imported functions to get the index of our compiled trace
            builder.appendULeb(builder.importedFunctionCount + i);
        }
        for (let i = 0; i < directQueue.length; i++) {
            const traceName = directQueue[i].getTraceName();
            builder.appendName(traceName + "_direct");
            builder.appendU8(0);
            builder.appendULeb(builder.importedFunctionCount + jitQueue.length + i);
        }

        // Code section
        builder.beginSection(10);
        builder.appendULeb(jitQueue.length + directQueue.length);
        for (let i = 0; i < jitQueue.length; i++) {
            const info = jitQueue[i];
            const traceName = info.getTraceName();
            builder.beginFunction(traceName, {
                "sp_args": WasmValtype.i32,
                "need_unbox": WasmValtype.i32,
                // wasm-JIT direct-forward scratch (unused, hence free, when the fast path is not emitted)
                "wj_fslot": WasmValtype.i32,
                // "void" is not a legal local type, and a void-returning target has nothing to stash
                "wj_ret": (info.wjNargs >= 0 && info.hasReturnValue) ? info.wjVtypes[info.wjNargs] : WasmValtype.i32,
            });

            const ok = generate_wasm_body(builder, info);
            if (!ok)
                throw new Error(`Failed to generate ${traceName}`);

            builder.appendU8(WasmOpcode.end);
            builder.endFunction(true);
        }
        for (let i = 0; i < directQueue.length; i++) {
            const info = directQueue[i];
            const traceName = info.getTraceName();
            builder.beginFunction(traceName, {
                "wj_fslot": WasmValtype.i32,
                "wj_ret": info.hasReturnValue ? info.wjVtypes[info.wjNargs] : WasmValtype.i32,
            });

            generate_wasm_jit_direct_body(builder, info);

            builder.appendU8(WasmOpcode.end);
            builder.endFunction(true);
        }

        builder.endSection();

        compileStarted = _now();
        const buffer = builder.getArrayView();
        if (trace > 0)
            mono_log_info(`jit queue generated ${buffer.length} byte(s) of wasm`);
        modifyCounter(JiterpCounter.BytesGenerated, buffer.length);
        const traceModule = new WebAssembly.Module(buffer);
        const wasmImports = builder.getWasmImports();

        const traceInstance = new WebAssembly.Instance(traceModule, wasmImports);

        // Now that we've jitted the trampolines, go through and fix up the function pointers
        //  to point to the new jitted trampolines instead of the default implementations
        for (let i = 0; i < jitQueue.length; i++) {
            const info = jitQueue[i];
            const traceName = info.getTraceName();

            // Get the exported guarded trampoline.
            const fn = traceInstance.exports[traceName];
            // Patch the function pointer for this function to use the trampoline now.
            fnTable.set(info.result, fn);
            info.guardedImplementation = fn as Function;
            info.directInstalled = false;
            if (info.wjNargs >= 0) {
                info.directImplementation = traceInstance.exports[traceName + "_direct"] as Function;
                // Admission may have happened before this wrapper crossed its compilation threshold.
                // In that ordering, install the adapter immediately instead of waiting for another
                // native/AOT entry to discover an already-live f-slot.
                if (cwraps.mono_jiterp_wasm_jit_admitted(info.imethod))
                    mono_jiterp_wasm_jit_patch_interp_entry(info.imethod);
            }

            rejected = false;
        }
        modifyCounter(JiterpCounter.EntryWrappersCompiled, jitQueue.length);
    } catch (exc: any) {
        threw = true;
        rejected = false;
        // console.error(`${traceName} failed: ${exc} ${exc.stack}`);
        // HACK: exc.stack is enormous garbage in v8 console
        mono_log_error(`interp_entry code generation failed: ${exc}`);
        recordFailure();
    } finally {
        const finished = _now();
        if (compileStarted) {
            modifyCounter(JiterpCounter.ElapsedGenerationMs, compileStarted - started);
            modifyCounter(JiterpCounter.ElapsedCompilationMs, finished - compileStarted);
        } else {
            modifyCounter(JiterpCounter.ElapsedGenerationMs, finished - started);
        }

        if (threw || (!rejected && ((trace >= 2) || dumpWrappers))) {
            mono_log_info(`// ${jitQueue.length} trampolines generated, blob follows //`);
            let s = "", j = 0;
            try {
                if (builder.inSection)
                    builder.endSection();
            } catch {
                // eslint-disable-next-line @typescript-eslint/no-extra-semi
                ;
            }

            const buf = builder.getArrayView(false, true);
            for (let i = 0; i < buf.length; i++) {
                const b = buf[i];
                if (b < 0x10)
                    s += "0";
                s += b.toString(16);
                s += " ";
                if ((s.length % 10) === 0) {
                    mono_log_info(`${j}\t${s}`);
                    s = "";
                    j = i + 1;
                }
            }
            mono_log_info(`${j}\t${s}`);
            mono_log_info("// end blob //");
        } else if (rejected && !threw) {
            mono_log_error("failed to generate trampoline for unknown reason");
        }
    }
}

function append_stackval_from_data (
    builder: WasmBuilder, imethod: number, type: MonoType, valueName: string, argIndex: number
) {
    const rawSize = cwraps.mono_jiterp_type_get_raw_value_size(type);
    const offset = cwraps.mono_jiterp_get_arg_offset(imethod, 0, argIndex);

    switch (rawSize) {
        case 256: {
            // Copy pointers directly
            builder.local("sp_args");
            builder.local(valueName);

            builder.appendU8(WasmOpcode.i32_store);
            builder.appendMemarg(offset, 2);
            break;
        }

        case -1:
        case -2:
        case 1:
        case 2:
        case 4: {
            // De-reference small primitives and then store them directly
            builder.local("sp_args");
            builder.local(valueName);

            switch (rawSize) {
                case -1:
                    builder.appendU8(WasmOpcode.i32_load8_u);
                    builder.appendMemarg(0, 0);
                    break;
                case 1:
                    builder.appendU8(WasmOpcode.i32_load8_s);
                    builder.appendMemarg(0, 0);
                    break;
                case -2:
                    builder.appendU8(WasmOpcode.i32_load16_u);
                    builder.appendMemarg(0, 0);
                    break;
                case 2:
                    builder.appendU8(WasmOpcode.i32_load16_s);
                    builder.appendMemarg(0, 0);
                    break;
                case 4:
                    builder.appendU8(WasmOpcode.i32_load);
                    builder.appendMemarg(0, 2);
                    break;
                // FIXME: 8-byte ints (unaligned)
                // FIXME: 4 and 8-byte floats (unaligned)
            }

            builder.appendU8(WasmOpcode.i32_store);
            builder.appendMemarg(offset, 2);
            break;
        }

        default: {
            // Call stackval_from_data to copy the value
            builder.ptr_const(type);
            // result
            builder.local("sp_args");
            // apply offset
            builder.i32_const(offset);
            builder.appendU8(WasmOpcode.i32_add);
            // value
            builder.local(valueName);

            builder.callImport("stackval_from_data");
            break;
        }
    }
}

/*
 * Guard-free native/AOT -> wasm-JIT adapter. It is never installed until mono_wasm_jit_admit has
 * installed and admitted the target's complete direct-call closure in this worker. Consequently the
 * per-call fslot==0 and slot-live bitmap guards from generate_wasm_body are admission-time work now.
 *
 * Keep loading wasm_jit_fslot rather than baking it: tiering forwards to a replacement InterpMethod
 * while preserving the published slot, and reading the canonical rmethod field keeps that contract
 * explicit without adding a branch.
 */
function generate_wasm_jit_direct_body (builder: WasmBuilder, info: TrampolineInfo) {
    mono_assert(info.wjNargs >= 0, "direct adapter without wasm-jit signature");

    builder.local("rmethod");
    builder.i32_const(~0x1);
    builder.appendU8(WasmOpcode.i32_and);
    builder.appendU8(WasmOpcode.i32_load);
    builder.appendMemarg(getMemberOffset(JiterpMember.WasmJitFslot), 2);
    builder.local("wj_fslot", WasmOpcode.set_local);

    // The trampoline ABI passes pointers; the scalar wasm-JIT body takes values.
    for (let i = 0; i < info.wjNargs; i++) {
        const isThis = info.hasThisReference && (i === 0);
        const valueName = isThis ? "this_arg" : `arg${i - (info.hasThisReference ? 1 : 0)}`;
        builder.local(valueName);
        switch (info.wjKinds[i]) {
            case 8: break; // already a value
            case 0: builder.appendU8(WasmOpcode.i32_load); builder.appendMemarg(0, 2); break;
            case 1: builder.appendU8(WasmOpcode.i64_load); builder.appendMemarg(0, 3); break;
            case 2: builder.appendU8(WasmOpcode.f32_load); builder.appendMemarg(0, 2); break;
            case 3: builder.appendU8(WasmOpcode.f64_load); builder.appendMemarg(0, 3); break;
            case 4: builder.appendU8(WasmOpcode.i32_load8_s); builder.appendMemarg(0, 0); break;
            case 5: builder.appendU8(WasmOpcode.i32_load8_u); builder.appendMemarg(0, 0); break;
            case 6: builder.appendU8(WasmOpcode.i32_load16_s); builder.appendMemarg(0, 1); break;
            case 7: builder.appendU8(WasmOpcode.i32_load16_u); builder.appendMemarg(0, 1); break;
            default: throw new Error(`bad wasm-jit load kind ${info.wjKinds[i]}`);
        }
    }

    builder.local("wj_fslot");
    builder.call_indirect(info.getTraceName() + "_wj", 0);

    if (info.hasReturnValue) {
        const rv = info.wjVtypes[info.wjNargs];
        builder.local("wj_ret", WasmOpcode.set_local);
        builder.local("res");
        builder.local("wj_ret");
        switch (rv) {
            case WasmValtype.i64: builder.appendU8(WasmOpcode.i64_store); builder.appendMemarg(0, 3); break;
            case WasmValtype.f32: builder.appendU8(WasmOpcode.f32_store); builder.appendMemarg(0, 2); break;
            case WasmValtype.f64: builder.appendU8(WasmOpcode.f64_store); builder.appendMemarg(0, 3); break;
            default: builder.appendU8(WasmOpcode.i32_store); builder.appendMemarg(0, 2); break;
        }
    }
}

function generate_wasm_body (
    builder: WasmBuilder, info: TrampolineInfo
): boolean {
    const paramsCount = info.paramTypes.length + (info.hasThisReference ? 1 : 0);

    // the this-reference may be a boxed struct that needs to be unboxed, for example calling
    //  methods like object.ToString on structs will end up with the unbox flag set
    // instead of passing an extra 'unbox' argument to every wrapper, though, the flag is hidden
    //  inside the rmethod/imethod parameter in the lowest bit (1), so we need to check it
    if (info.hasThisReference) {
        builder.block();
        // Find the unbox-this-reference flag in rmethod
        builder.local("rmethod");
        builder.i32_const(0x1);
        builder.appendU8(WasmOpcode.i32_and);
        // If the flag is not set (rmethod & 0x1) == 0 then skip the unbox operation
        builder.appendU8(WasmOpcode.i32_eqz);
        builder.appendU8(WasmOpcode.br_if);
        builder.appendULeb(0);
        // otherwise, the flag was set, so unbox the this reference and update the local
        builder.local("this_arg");
        builder.callImport("unbox");
        builder.local("this_arg", WasmOpcode.set_local);
        builder.endBlock();
    }

    /*
     * wasm-JIT direct forward. When the target is already JITted and its module is instantiated on THIS
     * thread, call its `f` straight from here: the prologue (thread-local header + context lookup), the
     * marshalling of every argument into the interp stack, and the whole of mono_jiterp_interp_entry are
     * all setup for an interpreter run that will not happen. perf annotate shows interp_entry's cost is
     * flat across ~74 instructions with no hotspot, i.e. the preamble IS the cost.
     *
     * Both runtime guards are load-bearing:
     *   fslot != 0            the method may not be JITted yet, and becomes so later - so this is a
     *                         per-call load, not a generation-time decision, and self-heals.
     *   descriptor admission  the function table is PER-THREAD and automatic rebatching reuses slots.
     *                          This rejects both an unsynced worker and a stale slot whose current
     *                          generation's complete dependency union has not yet been admitted.
     * Missing either just falls through to the original path below, which syncs and works as before.
     */
    if (info.wjNargs >= 0) {
        builder.block();

        // fslot = rmethod->wasm_jit_fslot  (rmethod's low bit is the unbox flag; masked off)
        builder.local("rmethod");
        builder.i32_const(~0x1);
        builder.appendU8(WasmOpcode.i32_and);
        builder.appendU8(WasmOpcode.i32_load);
        builder.appendMemarg(getMemberOffset(JiterpMember.WasmJitFslot), 2);
        builder.local("wj_fslot", WasmOpcode.tee_local);
        builder.appendU8(WasmOpcode.i32_eqz);
        builder.appendU8(WasmOpcode.br_if);
        builder.appendULeb(0);

        // ...and only if this worker admitted rmethod's current registry generation:
        builder.local("rmethod");
        builder.i32_const(~0x1);
        builder.appendU8(WasmOpcode.i32_and);
        builder.callImport("wasm_jit_admitted");
        builder.appendU8(WasmOpcode.i32_eqz);
        builder.appendU8(WasmOpcode.br_if);
        builder.appendULeb(0);

        // load each argument by value; the trampoline receives pointers, `f` takes values
        for (let i = 0; i < info.wjNargs; i++) {
            const isThis = info.hasThisReference && (i === 0);
            const valueName = isThis ? "this_arg" : `arg${i - (info.hasThisReference ? 1 : 0)}`;
            builder.local(valueName);
            switch (info.wjKinds[i]) {
                case 8: break; // already a value
                case 0: builder.appendU8(WasmOpcode.i32_load); builder.appendMemarg(0, 2); break;
                case 1: builder.appendU8(WasmOpcode.i64_load); builder.appendMemarg(0, 3); break;
                case 2: builder.appendU8(WasmOpcode.f32_load); builder.appendMemarg(0, 2); break;
                case 3: builder.appendU8(WasmOpcode.f64_load); builder.appendMemarg(0, 3); break;
                case 4: builder.appendU8(WasmOpcode.i32_load8_s); builder.appendMemarg(0, 0); break;
                case 5: builder.appendU8(WasmOpcode.i32_load8_u); builder.appendMemarg(0, 0); break;
                case 6: builder.appendU8(WasmOpcode.i32_load16_s); builder.appendMemarg(0, 1); break;
                case 7: builder.appendU8(WasmOpcode.i32_load16_u); builder.appendMemarg(0, 1); break;
                default: throw new Error(`bad wasm-jit load kind ${info.wjKinds[i]}`);
            }
        }

        builder.local("wj_fslot");
        builder.call_indirect(info.getTraceName() + "_wj", 0);

        // store the result where the caller expects it, then we are done
        if (info.hasReturnValue) {
            const rv = info.wjVtypes[info.wjNargs];
            builder.local("wj_ret", WasmOpcode.set_local);
            builder.local("res");
            builder.local("wj_ret");
            switch (rv) {
                case WasmValtype.i64: builder.appendU8(WasmOpcode.i64_store); builder.appendMemarg(0, 3); break;
                case WasmValtype.f32: builder.appendU8(WasmOpcode.f32_store); builder.appendMemarg(0, 2); break;
                case WasmValtype.f64: builder.appendU8(WasmOpcode.f64_store); builder.appendMemarg(0, 3); break;
                default: builder.appendU8(WasmOpcode.i32_store); builder.appendMemarg(0, 2); break;
            }
        }
        builder.appendU8(WasmOpcode.return_);

        builder.endBlock();
    }

    // prologue(rmethod, this_arg, params_count) -> sp_args
    // The prologue uses a thread-local JiterpEntryData for header state and delegate-invoke
    //  caching, so no per-wrapper scratch buffer needs to be passed in. This is what makes
    //  the wrapper safe to invoke concurrently from multiple threads.

    builder.local("rmethod");
    // Clear the unbox-this-reference flag if present (see above) so that rmethod is a valid ptr
    builder.i32_const(~0x1);
    builder.appendU8(WasmOpcode.i32_and);

    // prologue takes this_arg so it can handle delegates
    if (info.hasThisReference)
        builder.local("this_arg");
    else
        builder.i32_const(0);
    builder.i32_const(paramsCount);
    builder.callImport("interp_entry_prologue");
    builder.local("sp_args", WasmOpcode.set_local);

    /*
    if (sig->hasthis) {
        sp_args->data.p = data->this_arg;
        sp_args++;
    }
    */

    if (info.hasThisReference) {
        // null type for raw ptr copy
        append_stackval_from_data(builder, info.imethod, <any>0, "this_arg", 0);
    }

    /*
    for (i = 0; i < sig->param_count; ++i) {
        if (m_type_is_byref (sig->params [i])) {
            sp_args->data.p = params [i];
            sp_args++;
        } else {
            int size = stackval_from_data (sig->params [i], sp_args, params [i], FALSE);
            sp_args = STACK_ADD_BYTES (sp_args, size);
        }
    }
    */

    for (let i = 0; i < info.paramTypes.length; i++) {
        const type = <any>info.paramTypes[i];
        append_stackval_from_data(builder, info.imethod, type, `arg${i}`, i + (info.hasThisReference ? 1 : 0));
    }

    if (info.hasReturnValue)
        builder.local("res");
    else
        builder.i32_const(0);
    builder.callImport("interp_entry");
    builder.appendU8(WasmOpcode.return_);

    return true;
}
