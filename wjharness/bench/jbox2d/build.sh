#!/usr/bin/env bash
# Build the jbox2d fixed-work benchmark's arms from wjharness/bench/jbox2d/src.
#
#   wj bench/jbox2d/build [--out DIR] [--install]
#
# Writes (default DIR = $WJ_STATE/arms/j2d):
#   DIR/bench.jar        the jar ours (ikvmcraft ?bench), HotSpot (j2d/run hs<N>) and CheerpJ (cj<V>) run: the bench
#                        classes + jbox2d-library 2.2.1.1, Main-Class IkvmBenchmarkStarter, class files 52 (Java 8, so
#                        CheerpJ's Java 8 runtime takes it)
#   DIR/teavm-full/      TeaVM linear-memory wasm, optimization FULL  (j2d/run tvfull;   R180 arm A)
#   DIR/teavm-simple/    the same at optimization SIMPLE              (j2d/run tvsimple; R180 arm B, passes off)
#   DIR/BUILD.txt        inputs and hashes
# --install copies bench.jar to $IKVMCRAFT/frontend/public/bench.jar (what ?bench serves) and to
# $WJ_STATE/arms/cheerpj/bench.jar, keeping the replaced files as *.prev.
#
# The TeaVM arms are what the gradle sample in TeaVM's samples/benchmark built for R180-R527 (FULL = its wasm
# optimization AGGRESSIVE, SIMPLE = NONE; heap 1-16 MB; wasm exceptions on, the plugin's convention), here through the
# pinned CLI (wj bench/teavm-cli), with the schedule passed at run time instead of compiled in.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../../lib/env.sh"
OUT="$WJ_STATE/arms/j2d"; INSTALL=0
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT=$2; shift 2 ;;
    --install) INSTALL=1; shift ;;
    *) echo "usage: build.sh [--out DIR] [--install]" >&2; exit 1 ;;
  esac
done
# bench.jar is compiled for Java 8 (class files 52). Without a JDK 8 (Apple silicon has no Oracle/OpenJDK 8 build in
# reach of brew without a pkg installer), javac 21 --release 8 builds it: same class version, but javac's output may
# differ from the byte-identical jar the R180-R527 rounds ran, so BUILD.txt names the compiler and --install refuses.
if [ -n "${JAVA_HOME_8:-}" ]; then JDK8=$JAVA_HOME_8; REL8=()
else JDK8=$(need JAVA_HOME_21 "no JAVA_HOME_8: javac 21 --release 8 stands in"); REL8=(--release 8); fi
JBOX=~/.m2/repository/org/jbox2d/jbox2d-library/2.2.1.1/jbox2d-library-2.2.1.1.jar
if [ ! -f "$JBOX" ]; then
  mkdir -p "$(dirname "$JBOX")"
  curl -fsSL -o "$JBOX" https://repo1.maven.org/maven2/org/jbox2d/jbox2d-library/2.2.1.1/jbox2d-library-2.2.1.1.jar
fi
if [ -f "$JBOX.sha1" ] && [ "$(sha1sum "$JBOX" | cut -d' ' -f1)" != "$(cut -c1-40 "$JBOX.sha1")" ]; then
  echo "$JBOX does not match its .sha1" >&2; exit 1
fi

mkdir -p "$OUT"
TMP=$(mktemp -d "$WJ_RUNS/.tmp-j2d-XXXXXX" 2>/dev/null || { mkdir -p "$WJ_RUNS"; mktemp -d "$WJ_RUNS/.tmp-j2d-XXXXXX"; })
trap 'rm -rf "$TMP"' EXIT

# ---- bench.jar (javac 8; jar-with-dependencies layout: jbox2d's classes unpacked beside ours)
mkdir -p "$TMP/classes" "$TMP/fat"
"$JDK8/bin/javac" ${REL8[@]+"${REL8[@]}"} -g -nowarn -encoding UTF-8 -cp "$JBOX" -d "$TMP/classes" $(find "$HERE/src" -name '*.java' -not -path '*/benchmark/teavm/*')
( cd "$TMP/fat" && "$JDK8/bin/jar" xf "$JBOX" && rm -rf META-INF )
cp -r "$TMP/classes/." "$TMP/fat/"
printf 'Main-Class: org.teavm.samples.benchmark.ikvm.IkvmBenchmarkStarter\n' > "$TMP/manifest"
( cd "$TMP/fat" && find . -exec touch -d '1980-02-01 00:00:00' {} + && "$JDK8/bin/jar" cfm "$OUT/bench.jar" "$TMP/manifest" . )

# ---- TeaVM arms
mkdir -p "$TMP/tclasses"
"$JDK8/bin/javac" ${REL8[@]+"${REL8[@]}"} -g -nowarn -encoding UTF-8 -cp "$JBOX" -d "$TMP/tclasses" $(find "$HERE/src" -name '*.java' -not -path '*/benchmark/ikvm/*')
for lvl in full simple; do
  O=$([ $lvl = full ] && echo 3 || echo 1)
  rm -rf "$OUT/teavm-$lvl"; mkdir -p "$OUT/teavm-$lvl/wasm"
  bash "$HERE/../teavm.sh" -t wasm -O "$O" --wasm-use-exceptions \
    --min-heap 1 --max-heap 16 -p "$TMP/tclasses" -p "$JBOX" -d "$OUT/teavm-$lvl/wasm" -f benchmark.wasm \
    org.teavm.samples.benchmark.teavm.WasmFixedWorkBenchmarkStarter > "$TMP/teavm-$lvl.log" 2>&1 \
    || { cat "$TMP/teavm-$lvl.log" >&2; exit 1; }
  cp "$HERE/web/teavm-wasm-fixed.html" "$OUT/teavm-$lvl/"
done

{
  echo "jbox2d arms built $(isodate) from wjharness/bench/jbox2d ($(git -C "$WJ_HARNESS" rev-parse --short HEAD 2>/dev/null || echo '?'))"
  echo "javac: $("$JDK8/bin/javac" -version 2>&1)${REL8[@]+ ${REL8[*]}}"
  echo "jbox2d-library 2.2.1.1 sha1 $(sha1sum "$JBOX" | cut -d' ' -f1)"
  echo "$(head -1 "$WJ_STATE/arms/teavm-cli/BUILD.txt")"
  for f in bench.jar teavm-full/wasm/benchmark.wasm teavm-simple/wasm/benchmark.wasm; do
    echo "$f $(fsize "$OUT/$f") B sha256 $(sha256sum "$OUT/$f" | cut -d' ' -f1)"
  done
} | tee "$OUT/BUILD.txt"

if [ $INSTALL = 1 ] && [ ${#REL8[@]} -gt 0 ]; then
  echo "not installing: bench.jar was built by javac 21 --release 8, not the JDK 8 jar ikvmcraft serves" >&2
elif [ $INSTALL = 1 ]; then
  for dst in "$(need IKVMCRAFT)/frontend/public/bench.jar" "$WJ_STATE/arms/cheerpj/bench.jar"; do
    [ -f "$dst" ] && ! cmp -s "$dst" "$OUT/bench.jar" && cp -p "$dst" "$dst.prev"
    cp "$OUT/bench.jar" "$dst" && echo "installed $dst"
  done
fi
