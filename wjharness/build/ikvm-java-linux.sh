#!/bin/bash
# ikvm-java-linux.sh [--install] -- build IKVM.Java (and with it the ikvmc it needs) in a linux/amd64 container, for a
# host that cannot (macOS: IKVM.OpenJdk.Make configures OpenJDK for the HOST, so a Mac would compile the macOS java.base,
# while IKVM.Java.runtime.linux needs the Linux one). Needs docker with an amd64 runtime (colima: `colima start --vm-type
# vz --vz-rosetta`) and ext/openjdk checked out (`git submodule update --init --depth 1 ext/openjdk` in $IKVM_SRC).
# The OpenJDK build stays in $IKVM_SRC/artifacts, so a rerun only redoes what changed. --install stages the DLL like
# wj ikvm-runtime --java (statics/ikvm/ and statics/ikvm.zip; the replaced one kept under $WJ_STATE/arms/ikvm-java/).
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/../lib/env.sh"
SRC=$(need IKVM_SRC "the IKVM fork checkout")
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/ikvm-java-linux"
[ -f "$SRC/ext/openjdk/configure" ] || { echo "no $SRC/ext/openjdk: git submodule update --init --depth 1 ext/openjdk" >&2; exit 1; }
docker build -q -t ikvm-java-build "$HERE" >/dev/null
docker run --rm --platform linux/amd64 -v "$SRC:/ikvm" -v ikvm-nuget:/root/.nuget -v "$HERE/build.sh:/build.sh:ro" ikvm-java-build bash /build.sh
DLL="$SRC/artifacts/jdk25/bin/IKVM.Java.runtime.linux/Release/net8.0/linux-x64/IKVM.Java.dll"
[ -f "$DLL" ] || { echo "no $DLL" >&2; exit 1; }
echo "built   $(sha256sum "$DLL" | cut -c1-16)  $DLL"
[ "${1:-}" = "--install" ] || exit 0
APP=$(need IKVMCRAFT); old="$APP/statics/ikvm/IKVM.Java.dll"
mkdir -p "$WJ_STATE/arms/ikvm-java"; keep="$WJ_STATE/arms/ikvm-java/$(sha256sum "$old" | cut -c1-12).dll"
[ -f "$keep" ] || cp -p "$old" "$keep"
cp "$DLL" "$old"; tmp=$(mktemp -d); cp "$DLL" "$tmp/IKVM.Java.dll"; (cd "$tmp" && zip -q "$APP/statics/ikvm.zip" IKVM.Java.dll); rm -rf "$tmp"
echo "staged  $(sha256sum "$old" | cut -c1-16)  IKVM.Java.dll (kept $(basename "$keep")); now: wj deploy <marker>"
