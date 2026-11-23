#!/usr/bin/env bash
# The mctick TeaVM arm: wjharness/bench/mctick/{src,src-teavm} through the pinned TeaVM (bench/teavm-cli) into
# DIR/teavm-full/ (-O 3, the default) or DIR/teavm-simple/ (-O 1), each with wasm/mctick.wasm, its runtime JS,
# mctick.html and mctick-data.zip. Called by build.sh; alone:
#   wj bench/mctick/build-teavm [--out DIR] [--stage NAME] [-O 1|3] [-g]
#
# Class path order is what makes the shadows work: the bench classes (src + src-teavm, which carries mctick's TeaVM
# replacements for LogUtils, slf4j's factories, oshi's SystemInfo and netty's logger factory) come before client.jar
# and the libraries; log4j is left off. TeaVM's own classlib (patched, plus bench/teavm-classlib) comes first of all.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../../lib/env.sh"
OUT="$WJ_STATE/arms/mctick"; STAGE=sa262n; OPT=3; DEBUG=()
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT=$(wj_abspath "$2"); shift 2 ;;
    --stage) STAGE=$2; shift 2 ;;
    -O) OPT=$2; shift 2 ;;
    -g) DEBUG=(-g); shift ;;   # writes .wast and C dumps (~1 GB) beside the module; frames still print unnamed
    *) echo "usage: build-teavm.sh [--out DIR] [--stage NAME] [-O 1|3]" >&2; exit 1 ;;
  esac
done
JDK25=$(need JAVA_HOME_25 "Minecraft 26.2's class files are Java 25")
ST="$WJ_STATE/native/$STAGE"
CLI="$WJ_STATE/arms/teavm-cli/teavm-cli-9be5eca57.jar"
[ -f "$CLI" ] || { echo "no $CLI: run wj bench/teavm-cli" >&2; exit 1; }
LIBS=()
while IFS= read -r j; do
  case "$j" in
    *"/client.jar"|*/org/lwjgl/*|*-natives-*|*/net/fabricmc/*|*/org/ow2/asm/*|*/org/apache/logging/log4j/*) continue ;;
  esac
  LIBS+=("$j")
done < <(tr ':' '\n' < "$ST/cp.txt")
CP="$ST/client.jar:$(IFS=:; echo "${LIBS[*]}")"
CLS="$OUT/classes-teavm"; rm -rf "$CLS"; mkdir -p "$CLS"
"$JDK25/bin/javac" -g -nowarn -encoding UTF-8 -cp "$CLI:$CP" -d "$CLS" $(find "$HERE/src" "$HERE/src-teavm" -name '*.java')
( cd "$HERE/src-teavm" && find . -type f -not -name '*.java' | cp_parents "$CLS" )   # service files
# bytecode patches to Minecraft classes (teavm-patch/PatchClasses.java says which and why), with the stage's ASM
ASM=$(tr ':' '\n' < "$ST/cp.txt" | grep '/org/ow2/asm/asm/' | head -1)
ASMT=$(tr ':' '\n' < "$ST/cp.txt" | grep '/org/ow2/asm/asm-tree/' | head -1)
"$JDK25/bin/java" -cp "$ASM:$ASMT" "$HERE/teavm-patch/PatchClasses.java" "$CLS" "$ST/client.jar" "${LIBS[@]}"

# the arm's directory follows the optimization level: FULL -> teavm-full (tvfull), SIMPLE -> teavm-simple (tvsimple)
case "$OPT" in 3) ARMDIR=teavm-full ;; 1) ARMDIR=teavm-simple ;; *) ARMDIR=teavm-o$OPT ;; esac
D="$OUT/$ARMDIR"; rm -rf "$D"; mkdir -p "$D/wasm"
# ---- the data zip: class-path resources under res/, the save under world/
TMP=$(mktemp -d "$WJ_RUNS/.tmp-mctick-data-XXXXXX"); trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/res" "$TMP/world"
( cd "$TMP/res" && unzip -q "$ST/client.jar" 'data/*' 'assets/.mcassetsroot' 'assets/minecraft/lang/*' version.json )
cp -a "$ST/template/saves/New World" "$TMP/world/"
rm -f "$TMP/world/New World/session.lock"
( cd "$TMP" && find res world -exec touch -d '1980-02-01 00:00:00' {} + && zip -q -r -X -9 "$D/mctick-data.zip" res world )

# ---- TeaVM. Its linear-memory layout is int arithmetic (WasmTarget.renderMemoryLayout: static data + stack + heap +
# the GC's side tables, heap/16 of storage and the region tables), so the whole must stay under 2 GiB: with mctick's
# static data a 2000 MB heap already wraps and the module declares an impossible memory maximum. 1600 MB fits.
P=(-p "$CLS")
for j in "$ST/client.jar" "${LIBS[@]}"; do P+=(-p "$j"); done
TEAVM_JVM_OPTS="${TEAVM_JVM_OPTS:--Xmx9g -XX:+UseParallelGC}" bash "$HERE/../teavm.sh" -t wasm -O "$OPT" "${DEBUG[@]}" --wasm-use-exceptions \
  --min-heap 512 --max-heap 1600 "${P[@]}" -d "$D/wasm" -f mctick.wasm wjbench.mctick.Mctick 2>&1 | tee "$D/teavm.log" | tail -3
grep -q "^ERROR" "$D/teavm.log" && { echo "TeaVM reported $(grep -c '^ERROR' "$D/teavm.log") errors: $D/teavm.log" >&2; exit 1; }
cp "$HERE/web/mctick.html" "$D/"
ls -la "$D" "$D/wasm"
