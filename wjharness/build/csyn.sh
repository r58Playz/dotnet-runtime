#!/bin/sh
# Syntax-check ONE runtime source file with the exact emcc command line the real build uses, pulled from
# the CMake compile_commands.json left by the last configure. Under a second, versus ~35 min for a full
# `make build AOT=true`, and it catches everything a compile catches short of link errors.
#
# EVERY compilation database that names the file is checked, not just the first. That matters: mini-wasm.c
# is built twice, once for the runtime (HOST_BROWSER defined) and once into mono-aot-cross (not defined),
# and code placed inside an `#ifdef HOST_BROWSER` block compiles clean in the first and fails the second.
# Checking only the browser database let exactly that through to a 50-second build failure.
#
# Pre-existing -Wmissing-prototypes noise (34 of them in mini-wasm.c alone, all from EMSCRIPTEN_KEEPALIVE
# entry points with no header declaration) is filtered out, because a check whose clean output is 200 lines
# of warnings is a check nobody reads. Everything else is passed through.
#
# Usage: wj csyn src/mono/mono/mini/mini-wasm.c   (relative to the cwd, or absolute)
#
# WHAT THIS DOES NOT CHECK: LINKING. It compiles one file against both compilation databases, which
# catches syntax and type errors in both configurations -- and says nothing about whether the symbols
# resolve. R269 lost a 9-minute build to that gap: a new helper was added to mini-wasm.c INSIDE its
# `#ifdef HOST_BROWSER` region and called from interp.c without a guard. Both files compiled cleanly in
# both databases; the `mono-aot-cross` LINK then failed with `undefined symbol`, because mini-wasm.c and
# interp.c are both linked into the cross-compiler and HOST_BROWSER is not defined there.
#
# So: when adding a cross-file symbol, check which side of a HOST_BROWSER region each end sits on.
# CLAUDE.md records the same hazard in the other direction (MONO_WASM_JIT_DEVIRT_PROFILE lives in
# mini-wasm.c precisely because interp.c is not in the cross build's link for that symbol).
[ -n "$1" ] || { echo "usage: $0 <source file>" >&2; exit 2; }
root=$(cd "$(dirname "$0")/../.." && pwd)   # wjharness/build -> the repo
out=$(python3 - "$root" "$1" <<'PY' 2>&1
import json, os, shlex, subprocess, sys
root, want = sys.argv[1], os.path.abspath(sys.argv[2])
found = False
for db in ("artifacts/obj/mono/browser.wasm.Release/compile_commands.json",
           "artifacts/obj/mono/browser.wasm.Release/cross/compile_commands.json",
           "artifacts/obj/native/net10.0-browser-Release-wasm/compile_commands.json"):
    p = os.path.join(root, db)
    if not os.path.exists(p):
        continue
    for e in json.load(open(p)):
        if os.path.abspath(e["file"]) == want:
            argv, skip = [], False
            for a in shlex.split(e["command"]):
                if skip: skip = False; continue
                if a == "-o": skip = True; continue
                if a == "-c": continue
                argv.append(a)
            argv.insert(1, "-fsyntax-only")
            rc = subprocess.call(argv, cwd=e["directory"])
            print("--- checked:", db.split("/")[-2], flush=True)
            if rc: sys.exit(rc)
            found = True
if not found:
    print("no compile command found for", want, file=sys.stderr)
    sys.exit(2)
sys.exit(0)
PY
)
rc=$?
printf '%s\n' "$out" | awk '
  /warning: no previous prototype/ { skip=1; next }
  /^[0-9]+ warnings? generated\.$/ { next }
  skip && (/^ *[0-9]+ \|/ || /^ *\| */ || /note: declare .static./ || /^ *\^/ || /^ *static *$/) { next }
  { skip=0; if (length($0)) print }
'
[ $rc -eq 0 ] && echo "OK: $1"
exit $rc
