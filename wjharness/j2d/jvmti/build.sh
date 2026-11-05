#!/bin/bash
# Build the jbox2d HotSpot ledger's JVMTI agent (code bytes + inlining-aware pc maps at nmethod load, for
# j2d/hsledger.py) into $WJ_STATE/arms/jvmti/libj2dagent.so, where j2d/run.mjs loads it from.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/../../lib/env.sh"
JH=${JAVA_HOME:-$(need JAVA_HOME_25 "the agent compiles against a JDK's jvmti.h")}
out="$WJ_STATE/arms/jvmti"; mkdir -p "$out"
gcc -O2 -shared -fPIC -I"$JH/include" -I"$JH/include/linux" -o "$out/libj2dagent.so" "$(dirname "$0")/j2dagent.c"
echo "built $out/libj2dagent.so"
