#!/usr/bin/env bash
# Build the mctick (Minecraft 26.2 server tick) arms from wjharness/bench/mctick/.
#
#   wj bench/mctick/build [--out DIR] [--stage NAME] [--install] [--no-teavm]
#
# Inputs: the native stage $WJ_STATE/native/<stage> (default sa262n, built by wj native/mcstage from the 262n seed's
# tar): its client.jar (26.2, sha1-checked by mcstage), its class path, and its save (template/saves/New World).
# Writes (default DIR = $WJ_STATE/arms/mctick):
#   DIR/classes/         the bench compiled against 26.2 (javac 25: Minecraft's class files are 69)
#   DIR/cp-jvm.txt       the class path the JVM arms run (DIR/classes, client.jar, the vanilla libraries)
#   DIR/mctick.jar       the ours arm's jar: the bench classes, Main-Class wjbench.mctick.Launch, and
#                        wjbench/mctick/classpath.txt = the same libraries as paths under ikvmcraft's libraries/
#   DIR/teavm-full/      the TeaVM arm (linear-memory wasm, optimization FULL), unless --no-teavm
#   DIR/BUILD.txt
# --install copies mctick.jar to $IKVMCRAFT/frontend/public/ (what ?bench&jar=mctick.jar serves).
#
# The vanilla library set is the stage's class path minus what only the client or Fabric uses (LWJGL and its natives,
# Fabric Loader, Mixin, ASM, the intermediary mappings): a server needs none of them.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../../lib/env.sh"
OUT="$WJ_STATE/arms/mctick"; STAGE=sa262n; INSTALL=0; TEAVM=1
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT=$(wj_abspath "$2"); shift 2 ;;
    --stage) STAGE=$2; shift 2 ;;
    --install) INSTALL=1; shift ;;
    --no-teavm) TEAVM=0; shift ;;
    *) echo "usage: build.sh [--out DIR] [--stage NAME] [--install] [--no-teavm]" >&2; exit 1 ;;
  esac
done
JDK25=$(need JAVA_HOME_25 "Minecraft 26.2's class files are Java 25")
ST="$WJ_STATE/native/$STAGE"
[ -f "$ST/cp.txt" ] && [ -f "$ST/client.jar" ] || { echo "no stage $ST: run wj native/mcstage" >&2; exit 1; }
mkdir -p "$OUT"

# ---- the vanilla server's libraries, from the stage's class path
LIBS=()
while IFS= read -r j; do
  case "$j" in
    *"/client.jar"|*/org/lwjgl/*|*-natives-*|*/net/fabricmc/*|*/org/ow2/asm/*) continue ;;
  esac
  [ -f "$j" ] || { echo "stage class path names a missing jar: $j" >&2; exit 1; }
  LIBS+=("$j")
done < <(tr ':' '\n' < "$ST/cp.txt")
REL=(); for j in "${LIBS[@]}"; do REL+=("${j#*/libraries/}"); done

# ---- compile
rm -rf "$OUT/classes"; mkdir -p "$OUT/classes"
CP="$ST/client.jar:$(IFS=:; echo "${LIBS[*]}")"
"$JDK25/bin/javac" -g -nowarn -encoding UTF-8 -cp "$CP" -d "$OUT/classes" \
  $(find "$HERE/src" "$HERE/src-jvm" "$HERE/src-launch" -name '*.java')
echo "$OUT/classes:$CP" > "$OUT/cp-jvm.txt"

# ---- mctick.jar (ours)
TMP=$(mktemp -d "$WJ_RUNS/.tmp-mctick-XXXXXX"); trap 'rm -rf "$TMP"' EXIT
cp -r "$OUT/classes/." "$TMP/"
printf '# 26.2 vanilla server libraries, relative to <ikvmcraft root>/libraries (wjharness/bench/mctick/build.sh)\n' > "$TMP/wjbench/mctick/classpath.txt"
printf '%s\n' "${REL[@]}" >> "$TMP/wjbench/mctick/classpath.txt"
printf 'Main-Class: wjbench.mctick.Launch\n' > "$TMP/../mctick-manifest-$$"
( cd "$TMP" && find . -exec touch -d '1980-02-01 00:00:00' {} + && "$JDK25/bin/jar" cfm "$OUT/mctick.jar" "$TMP/../mctick-manifest-$$" . )
rm -f "$TMP/../mctick-manifest-$$"

# ---- TeaVM
if [ $TEAVM = 1 ]; then
  bash "$HERE/build-teavm.sh" --out "$OUT" --stage "$STAGE"
fi

{
  echo "mctick built $(isodate) from wjharness/bench/mctick ($(git -C "$WJ_HARNESS" rev-parse --short HEAD 2>/dev/null || echo '?'))"
  echo "stage $STAGE: $(grep '^client.jar' "$ST/STAGE.txt" 2>/dev/null || echo "client.jar sha1 $(sha1sum "$ST/client.jar" | cut -d' ' -f1)")"
  echo "libraries ${#LIBS[@]}: ${REL[*]}"
  echo "mctick.jar sha256 $(sha256sum "$OUT/mctick.jar" | cut -d' ' -f1)"
  [ -f "$OUT/teavm-full/wasm/mctick.wasm" ] && echo "teavm-full/wasm/mctick.wasm $(fsize "$OUT/teavm-full/wasm/mctick.wasm") B"
  true
} | tee "$OUT/BUILD.txt"

if [ $INSTALL = 1 ]; then
  cp "$OUT/mctick.jar" "$(need IKVMCRAFT)/frontend/public/mctick.jar" && echo "installed $IKVMCRAFT/frontend/public/mctick.jar"
fi
