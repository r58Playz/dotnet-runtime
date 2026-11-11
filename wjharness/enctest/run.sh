#!/bin/sh
# Host-side gate for the relocatable-body work. Compiles wasm-encoder.c against a tiny glib shim and
# checks the three identity properties the refactor rests on:
#   t1  wasm_module_assemble(n==1)  == wasm_module_method_and_entry      (the single-method framer)
#   t2  wasm_module_assemble(n>1)   == wasm_module_methods_and_entries   (the batch framer)
# Both oracles live in legacy.c -- frozen copies of the code the refactor deleted, not the live encoder.
#   t3  wasm_body_serialize with every call resolved to INDIRECT == the same body emitted with indices baked
#   t4  wasm_module_assemble(nexport < nmembers) is well-formed -- the SHADOW layout, which t1/t2 cannot
#       reach because both call the assembler with nexport == nmembers
#   t7  lazy stub banks: validated with wasm-validate and EXECUTED in node (t7.mjs) -- tail-call bind, interp leg,
#       throw, and the i32-param pin frame below s.p
#   t9  origin map (gap ledger): transitions on both sides of zero-width holes land exactly where a baked copy of
#       the body puts them; the wj.origin section names the same bytes, and the module is otherwise unchanged
#   t10 the guarded module-local call (plan2x S1): framed for three signatures (+ one PRIVATE callee, R452) and EXECUTED in node (t10.mjs) with
#       the guard word clear (the old copy, `call <funcidx>`) and set (the table's f-slot, and the taken-arm counter)
#   t11 compact virtual sites' shared slow-path stub (plan typed-spindle C1): framed for 48 functypes x every return
#       normalisation and EXECUTED in node (t11.mjs) through all 8 routes, incl. the pin frame and the i64-descriptor rule
#   t12 R516: the serializer's call-site table -- every call opcode written at a hole is recorded at its offset with
#       the tag its reloc carried, and nothing else (the managed stack walk maps V8 frame positions through it)
#   t13 R516: call-site KEYS -- every key is the memory access an independent model of the rule picks (the last one
#       before the call in its run, only when the run has one after it too; a guarded pair's guard load), no other
#   t14 the exnref EH region an EH method's dispatch is framed in (wasm_eh_region_open/_catch, wasm_eh_throw_payload):
#       validated and EXECUTED in node (t14.mjs) -- catch + re-dispatch (twice), an unclaimed x.e leaving with the same
#       payload, a JS exception passing through
#   t5  ONE body framed at two different ti_base values asks for the SAME functype -- the re-framing case
#       re-emission creates, which t1-t4 structurally cannot reach (they frame each body once). Carries
#       its own negative control: a deliberately off-by-one ti_base must be caught, or the gate is inert.
# Seconds to run, versus ~35 min for a build+deploy+bootcheck. Run it before believing anything else.
# Binaries and the modules they write go to $ENCTEST_OUT (default: a fresh temp dir, removed on success), never
# beside the sources.
set -e
d=$(cd "$(dirname "$0")" && pwd)
m=$(cd "$d/../../src/mono/mono/mini" && pwd)
o=${ENCTEST_OUT:-$(mktemp -d)}; mkdir -p "$o"
for t in t t2 t3 t4 t5 t6 t7 t9 t10 t11 t12 t13 t14; do
	gcc -g -O1 -Wall -I"$d" -I"$m" -o "$o/$t.bin" "$d/$t.c" "$d/legacy.c" "$m/wasm-encoder.c"
	(cd "$o" && "./$t.bin")
done
(cd "$o" && node "$d/t7.mjs")
(cd "$o" && node "$d/t10.mjs")
(cd "$o" && node "$d/t11.mjs")
(cd "$o" && node "$d/t14.mjs")
[ -n "${ENCTEST_OUT:-}" ] || rm -rf "$o"
