// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.IO;
using System.Reflection;
using System.Reflection.Emit;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;
using Microsoft.Build.Framework;
using WasmAppBuilder;

// Interp-to-native cookies for every `calli` through an UNMANAGED function pointer in the scanned assemblies.
//
// The interpreter reaches native code through a per-signature trampoline looked up by cookie
// (mono_wasm_get_interp_to_native_trampoline), and the table holds only what this task generates. Pinvokes and icalls
// are collected from metadata; a calli's signature is visible only in IL. A missing cookie is a g_error the first time
// the INTERPRETER runs that calli -- AOT'd code calls the pointer directly and never consults the table -- so an
// assembly that calls natives through function pointers (IKVM's JNI stubs, LWJGL's JNI.invoke*) is safe off AOT only
// if its calli shapes are here. Sites whose signature does not map to a cookie (non-primitive value types, generics)
// are counted and skipped, never guessed: a wrong cookie is a wrong trampoline.
internal sealed class CalliSignatureCollector
{
    private static readonly Dictionary<short, OperandType> s_operandTypes = BuildOperandTypes();
    private readonly LogAdapter _log;

    public HashSet<string> Signatures { get; } = new();

    public CalliSignatureCollector(LogAdapter log) => _log = log;

    private static Dictionary<short, OperandType> BuildOperandTypes()
    {
        var map = new Dictionary<short, OperandType>();
        foreach (FieldInfo f in typeof(OpCodes).GetFields(BindingFlags.Public | BindingFlags.Static))
        {
            if (f.GetValue(null) is OpCode op)
                map[op.Value] = op.OperandType;
        }
        return map;
    }

    public void ScanAssembly(string path)
    {
        int sites = 0, skipped = 0, added = 0;
        try
        {
            using var stream = File.OpenRead(path);
            using var pe = new PEReader(stream);
            if (!pe.HasMetadata)
                return;
            MetadataReader md = pe.GetMetadataReader();
            var provider = new CookieTypeProvider();
            foreach (MethodDefinitionHandle mh in md.MethodDefinitions)
            {
                MethodDefinition method = md.GetMethodDefinition(mh);
                if (method.RelativeVirtualAddress == 0)
                    continue;
                MethodBodyBlock body = pe.GetMethodBody(method.RelativeVirtualAddress);
                BlobReader il = body.GetILReader();
                while (il.RemainingBytes > 0)
                {
                    short code = il.ReadByte();
                    if (code == 0xFE)
                        code = unchecked((short)(0xFE00 | il.ReadByte()));
                    if (!s_operandTypes.TryGetValue(code, out OperandType operand))
                        break;   // not IL we understand; stop scanning this body rather than misread it
                    if (code == OpCodes.Calli.Value)
                    {
                        int token = il.ReadInt32();
                        if (TryCookie(md, provider, token, out string? cookie))
                        {
                            sites++;
                            if (Signatures.Add(cookie!))
                                added++;
                        }
                        else if (cookie != null)
                        {
                            sites++;
                            skipped++;
                            _log.LogMessage(MessageImportance.Low, $"calli in {md.GetString(method.Name)}: unsupported signature {cookie}, no interp-to-native cookie");
                        }
                        continue;
                    }
                    SkipOperand(ref il, operand);
                }
            }
        }
        catch (Exception ex) when (ex is BadImageFormatException || ex is InvalidOperationException)
        {
            _log.Warning("WASM0065", $"Could not scan '{path}' for calli signatures: {ex.Message}");
            return;
        }
        if (sites > 0)
            _log.LogMessage(MessageImportance.Normal, $"{Path.GetFileName(path)}: {sites} unmanaged calli sites, {added} new interp-to-native cookies, {skipped} skipped");
    }

    private static void SkipOperand(ref BlobReader il, OperandType operand)
    {
        switch (operand)
        {
            case OperandType.InlineNone:
                break;
            case OperandType.ShortInlineBrTarget:
            case OperandType.ShortInlineI:
            case OperandType.ShortInlineVar:
                il.Offset += 1;
                break;
            case OperandType.InlineVar:
                il.Offset += 2;
                break;
            case OperandType.InlineI8:
            case OperandType.InlineR:
                il.Offset += 8;
                break;
            case OperandType.InlineSwitch:
            {
                // Read the count first: `il.Offset += 4 * il.ReadInt32()` loads Offset BEFORE ReadInt32 advances it,
                // dropping the count's own 4 bytes and desynchronising every instruction after a switch.
                int targets = il.ReadInt32();
                il.Offset += 4 * targets;
                break;
            }
            default:
                il.Offset += 4;
                break;
        }
    }

    // cookie = null and FALSE: a managed calli (nothing to do). cookie set and FALSE: unmanaged, but unmappable.
    private static bool TryCookie(MetadataReader md, CookieTypeProvider provider, int token, out string? cookie)
    {
        cookie = null;
        EntityHandle h = MetadataTokens.EntityHandle(token);
        if (h.Kind != HandleKind.StandaloneSignature)
            return false;
        StandaloneSignature sig = md.GetStandaloneSignature((StandaloneSignatureHandle)h);
        if (sig.GetKind() != StandaloneSignatureKind.Method)
            return false;
        MethodSignature<char> ms = sig.DecodeMethodSignature(provider, null);
        SignatureCallingConvention cc = ms.Header.CallingConvention;
        if (cc == SignatureCallingConvention.Default || cc == SignatureCallingConvention.VarArgs)
            return false;
        var sb = new System.Text.StringBuilder();
        bool ok = ms.ReturnType != '?';
        sb.Append(ms.ReturnType);
        if (ms.Header.IsInstance)
            sb.Append('I');
        foreach (char p in ms.ParameterTypes)
        {
            ok &= p != '?' && p != 'V';
            sb.Append(p);
        }
        cookie = sb.ToString();
        return ok;
    }

    // Maps a signature type to aot-runtime-wasm.c type_to_c's letter. '?' = not decidable from the signature alone
    // (a value type that may be an enum or a single-scalar struct): the caller skips the site.
    private sealed class CookieTypeProvider : ISignatureTypeProvider<char, object?>
    {
        public char GetPrimitiveType(PrimitiveTypeCode typeCode) => typeCode switch
        {
            PrimitiveTypeCode.Void => 'V',
            PrimitiveTypeCode.Int64 or PrimitiveTypeCode.UInt64 => 'L',
            PrimitiveTypeCode.Single => 'F',
            PrimitiveTypeCode.Double => 'D',
            PrimitiveTypeCode.TypedReference => '?',
            _ => 'I',   // bool, char, (u)int8/16/32, (u)intptr, string, object
        };
        public char GetTypeFromDefinition(MetadataReader reader, TypeDefinitionHandle handle, byte rawTypeKind) => rawTypeKind == 0x11 ? '?' : 'I';
        public char GetTypeFromReference(MetadataReader reader, TypeReferenceHandle handle, byte rawTypeKind) => rawTypeKind == 0x11 ? '?' : 'I';
        public char GetTypeFromSpecification(MetadataReader reader, object? genericContext, TypeSpecificationHandle handle, byte rawTypeKind) => rawTypeKind == 0x11 ? '?' : 'I';
        public char GetSZArrayType(char elementType) => 'I';
        public char GetArrayType(char elementType, ArrayShape shape) => 'I';
        public char GetByReferenceType(char elementType) => 'I';
        public char GetPointerType(char elementType) => 'I';
        public char GetFunctionPointerType(MethodSignature<char> signature) => 'I';
        public char GetGenericInstantiation(char genericType, ImmutableArray<char> typeArguments) => genericType == '?' ? '?' : 'I';
        public char GetGenericMethodParameter(object? genericContext, int index) => '?';
        public char GetGenericTypeParameter(object? genericContext, int index) => '?';
        public char GetModifiedType(char modifier, char unmodifiedType, bool isRequired) => unmodifiedType;
        public char GetPinnedType(char elementType) => elementType;
    }
}
