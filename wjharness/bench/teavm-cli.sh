#!/usr/bin/env bash
# Build the TeaVM every TeaVM arm in this harness compiles with, pinned to the LAST commit that has TeaVM's
# linear-memory wasm backend, plus the harness's patches, into $WJ_STATE/arms/teavm-cli/.
#
#   wj bench/teavm-cli [--force]
#
# Why a pin and not upstream: TeaVM deleted its linear-memory ("legacy") wasm and WASI backends in d83fab4ae
# (2026-03-14, "Remove legacy Wasm and WASI backends"; the leftovers went in c552cbb9f), so master and every release
# from 0.14.0 on can emit only wasm-gc, JavaScript and C. Its parent, 9be5eca57 (2026-02-24), is the final state of the
# linear-memory backend, and it is what the jbox2d TeaVM arms of R180-R527 were built from. Nothing upstream changes
# that backend after it, so this pin IS the up-to-date linear-memory TeaVM.
#
# bench/teavm-patches/*.patch are applied in name order. They fix TeaVM bugs a large program (mctick) hits; each says
# whether it can change generated code. The list and the jar's hash land in BUILD.txt.
#
# The build runs in a worktree of $TEAVM_DIR at $WJ_RUNS/.tmp/teavm-cli-src (reset to the pin and re-patched every
# time, kept between builds so gradle's incremental build applies); whatever $TEAVM_DIR has checked out is untouched.
# Three defects of the shaded CLI jar at the pin are repaired below (commons-cli, the plugin services file) and one in
# bench/teavm.sh (the class path).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../lib/env.sh"
PIN=9be5eca57
OUT="$WJ_STATE/arms/teavm-cli"
JAR="$OUT/teavm-cli-$PIN.jar"
PATCHES=("$HERE"/teavm-patches/*.patch)
[[ -f "$JAR" && "${1:-}" != "--force" ]] && { echo "$JAR exists (--force rebuilds)"; exit 0; }
: "${TEAVM_DIR:?set TEAVM_DIR (a TeaVM checkout that has commit $PIN)}"
git -C "$TEAVM_DIR" cat-file -e "$PIN^{commit}" 2>/dev/null || { echo "TEAVM_DIR=$TEAVM_DIR lacks $PIN; git fetch there" >&2; exit 1; }
JH="${JAVA_HOME_21:-${JAVA_HOME_17:-}}"
[[ -x "$JH/bin/java" ]] || { echo "TeaVM $PIN builds with JDK 17 or 21: set JAVA_HOME_21" >&2; exit 1; }

WT="$WJ_RUNS/.tmp/teavm-cli-src"
mkdir -p "$WJ_RUNS/.tmp" "$OUT"
if [ ! -e "$WT/.git" ]; then
  git -C "$TEAVM_DIR" worktree prune
  git -C "$TEAVM_DIR" worktree add --detach "$WT" "$PIN" >/dev/null
fi
git -C "$WT" reset -q --hard "$PIN"   # ignored build outputs survive reset and clean, so gradle stays incremental
git -C "$WT" clean -fdq
for p in "${PATCHES[@]}"; do
  [ -f "$p" ] || continue
  git -C "$WT" apply --allow-empty "$p" || { echo "patch $(basename "$p") does not apply at $PIN" >&2; exit 1; }
done
# bench/teavm-classlib: JDK classes TeaVM's classlib lacks (single-threaded java.util.concurrent, FileChannel, ...),
# compiled as part of the classlib module so the patched classes and these can refer to each other. Classes outside
# java.* (sun.misc.Unsafe, javax.security.cert.X509Certificate) are not mapped by TeaVM and javac refuses them in that
# module: they go into teavm-extra.jar, built below, under their own names.
( cd "$HERE/teavm-classlib/src" && find . -name '*.java' -not -path './sun/*' -not -path './javax/*' | cp_parents "$WT/classlib/src/main/java" )
( cd "$WT" && JAVA_HOME="$JH" ./gradlew --no-daemon -q :tools:cli:shadowJar -x test -x javadoc )
EX="$WJ_RUNS/.tmp/teavm-extra"; rm -rf "$EX"; mkdir -p "$EX/classes"
"$JH/bin/javac" -nowarn --release 8 -d "$EX/classes" $(find "$HERE/teavm-classlib/src/sun" "$HERE/teavm-classlib/src/javax" -name '*.java')
( cd "$EX/classes" && "$JH/bin/jar" cf "$OUT/teavm-extra.jar" . )
rm -rf "$EX"
cp "$WT"/tools/cli/build/libs/cli-*-all.jar "$JAR"
# The shaded jar relocates commons-cli to org.teavm.apachecommons.cli but leaves TeaVMRunner referring to
# org.apache.commons.cli, so the runner needs the original beside it (bench/teavm.sh puts it on the classpath).
CC=$(find ~/.gradle/caches/modules-2 -path '*commons-cli/1.10.0/*' -name 'commons-cli-1.10.0.jar' | head -1)
[ -n "$CC" ] || { CC="$WJ_RUNS/.tmp/commons-cli-1.10.0.jar"; curl -fsSL -o "$CC" https://repo1.maven.org/maven2/commons-cli/commons-cli/1.10.0/commons-cli-1.10.0.jar; }
cp "$CC" "$OUT/commons-cli-1.10.0.jar"
# ...and its merged META-INF/services/org.teavm.vm.spi.TeaVMPlugin keeps only JCLPlugin, so JCLPlugin.install fails
# with "Service not registered: org.teavm.platform.plugin.MetadataRegistration". Write the four plugins of the modules
# it bundles (each module's own services file at $PIN) back into the jar.
SVC="$WJ_RUNS/.tmp/teavm-cli-svc"; rm -rf "$SVC"; mkdir -p "$SVC/META-INF/services"
printf '%s\n' org.teavm.classlib.impl.JCLPlugin org.teavm.platform.plugin.PlatformPlugin org.teavm.jso.impl.JSOPlugin \
  org.teavm.metaprogramming.impl.MetaprogrammingPlugin > "$SVC/META-INF/services/org.teavm.vm.spi.TeaVMPlugin"
( cd "$SVC" && "$JH/bin/jar" uf "$JAR" META-INF/services/org.teavm.vm.spi.TeaVMPlugin )
rm -rf "$SVC"
{
  echo "teavm-cli $PIN ($(git -C "$TEAVM_DIR" log -1 --format='%ci %s' "$PIN"))"
  for p in "${PATCHES[@]}"; do [ -f "$p" ] && echo "  + $(basename "$p") $(sha256sum "$p" | cut -c1-12)"; done
  echo "  + bench/teavm-classlib $(cd "$HERE/teavm-classlib/src" && find . -name '*.java' | sort | xargs cat | sha256sum | cut -c1-12) ($(find "$HERE/teavm-classlib/src" -name '*.java' | wc -l) files)"
  echo "built $(isodate) with $("$JH/bin/java" -version 2>&1 | head -1)"
  echo "sha256 $(sha256sum "$JAR" | cut -d' ' -f1)"
  echo "the last TeaVM commit with the linear-memory wasm backend (removed in d83fab4ae); see wjharness/bench/teavm-cli.sh"
} > "$OUT/BUILD.txt"
cat "$OUT/BUILD.txt"
