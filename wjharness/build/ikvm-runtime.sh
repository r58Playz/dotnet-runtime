#!/bin/bash
# ikvm-runtime.sh [--java] [--install] -- rebuild IKVM.Runtime (build-ikvm-local.sh's version properties, so the assembly
# identity matches the shipped CI build), and with --install stage it into ikvmcraft for deploy.sh.
# --java also rebuilds IKVM.Java (src/IKVM.Java.runtime.linux, which publishes ikvmc from these sources and compiles the
# class library with it): needed for anything that acts at IMPORT time -- map.xml bodies and prologues, core-library
# intrinsics (#if IMPORTER) -- which an IKVM.Runtime-only build never applies (R496, IKVM_PARK_FAST). Staged the same
# way; the replaced DLL is kept under $WJ_STATE/arms/ikvm-java/. The served IKVM.Java is split into chunks
# (IKVM.Java.<hash>.dll0..N): prove it with `cat` of the chunks | sha256sum against the hash printed here.
#
# Staging writes BOTH copies (R514): statics/ikvm/IKVM.Runtime.dll, which publish reads, and the entry in statics/ikvm.zip,
# because `make ikvmc-bundles` re-unzips that zip over the directory. The replaced DLL is kept under
# $WJ_STATE/arms/ikvm-runtime/<sha12>.dll. After deploy, prove the change is SERVED: deploy.sh checks the served
# IKVM.Runtime.*.dll byte count; compare its sha256 against the one printed here (an env-var name in it is UTF-16 in the
# #US heap, so a plain ASCII grep for a knob name finds nothing -- R372).
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/../lib/env.sh"
SRC=$(need IKVM_SRC "the IKVM fork checkout")
JAVA=0; INSTALL=0
for a in "$@"; do case "$a" in --java) JAVA=1 ;; --install) INSTALL=1 ;; *) echo "usage: wj ikvm-runtime [--java] [--install]" >&2; exit 2 ;; esac; done
cd "$SRC"
# ikvm-wasm-build's build-ikvm-local.sh is the reference (its dcf29b2 moved the cache to the user's default). The fork
# builds into artifacts/jdk25; src/IKVM.Runtime/bin is a STALE pre-jdk25 output and must not be picked up.
export NUGET_PACKAGES=${NUGET_PACKAGES:-$HOME/.nuget/packages}
export GitVersion_FullSemVer=8.14.0 GitVersion_SemVer=8.14.0 GitVersion_AssemblySemVer=8.14.0.0 GitVersion_AssemblySemFileVer=8.14.0.0 GitVersion_InformationalVersion=8.14.0 GitVersion_PreReleaseLabel= GitVersion_WeightedPreReleaseNumber=0
PROPS=(/p:Configuration=Release /p:EnabledRuntimes=linux-x64 /p:EnabledImageRuntimes=linux-x64 /p:EnabledImageBinRuntimes=linux-x64
  /p:EnabledToolRuntimes=linux-x64 /p:RuntimeIdentifier=linux-x64 /p:BuildNet6JreImages=false)
# IkvmToolReference is not a NuGet ProjectReference, so the runtime's /restore does not visit ikvmc or ikvmstub
# (NETSDK1112 "runtime pack ... was not downloaded" otherwise).
for t in src/ikvmc/ikvmc.csproj src/ikvmstub/ikvmstub.csproj; do dotnet msbuild /t:Restore /nr:false "${PROPS[@]}" "$t" >/dev/null; done
dotnet msbuild /m /nr:false /restore "${PROPS[@]}" /p:PreReleaseLabel= /p:PreReleaseNumber=0 /p:Version=8.14.0 \
  /p:AssemblyVersion=8.14.0.0 /p:InformationalVersion=8.14.0 /p:FileVersion=8.14.0.0 /p:PackageVersion=8.14.0 \
  /p:RepositoryUrl=https://github.com/ikvmnet/ikvm.git /p:PackageProjectUrl=https://github.com/ikvmnet/ikvm \
  /p:ContinuousIntegrationBuild=true src/IKVM.Runtime/IKVM.Runtime.csproj
# the newer of the RID-less and the RID output, as build-ikvm-local.sh's copy_managed_outputs picks it
DLL=""
for c in "$SRC"/artifacts/jdk25/bin/IKVM.Runtime/Release/net8.0/IKVM.Runtime.dll "$SRC"/artifacts/jdk25/bin/IKVM.Runtime/Release/net8.0/*/IKVM.Runtime.dll; do
  [ -f "$c" ] && { [ -z "$DLL" ] || [ "$c" -nt "$DLL" ]; } && DLL=$c
done
[ -n "$DLL" ] || { echo "no IKVM.Runtime.dll under $SRC/artifacts/jdk25/bin/IKVM.Runtime" >&2; exit 1; }
echo "built   $(sha256sum "$DLL" | cut -c1-16)  $DLL  ($(stat -c '%y' "$DLL" | cut -c1-19))"
JDLL=""
if [ "$JAVA" = 1 ]; then
  dotnet msbuild /m /nr:false /restore "${PROPS[@]}" /p:PreReleaseLabel= /p:PreReleaseNumber=0 /p:Version=8.14.0 \
    /p:AssemblyVersion=8.14.0.0 /p:InformationalVersion=8.14.0 /p:FileVersion=8.14.0.0 /p:PackageVersion=8.14.0 \
    /p:RepositoryUrl=https://github.com/ikvmnet/ikvm.git /p:PackageProjectUrl=https://github.com/ikvmnet/ikvm \
    /p:ContinuousIntegrationBuild=true src/IKVM.Java.runtime.linux/IKVM.Java.runtime.linux.ikvmproj
  for c in "$SRC"/artifacts/jdk25/bin/IKVM.Java.runtime.linux/Release/net8.0/IKVM.Java.dll "$SRC"/artifacts/jdk25/bin/IKVM.Java.runtime.linux/Release/net8.0/*/IKVM.Java.dll; do
    [ -f "$c" ] && { [ -z "$JDLL" ] || [ "$c" -nt "$JDLL" ]; } && JDLL=$c
  done
  [ -n "$JDLL" ] || { echo "no IKVM.Java.dll under $SRC/artifacts/jdk25/bin/IKVM.Java.runtime.linux" >&2; exit 1; }
  echo "built   $(sha256sum "$JDLL" | cut -c1-16)  $JDLL  ($(stat -c '%y' "$JDLL" | cut -c1-19))"
fi
[ "$INSTALL" = 1 ] || exit 0

APP=$(need IKVMCRAFT)
stage () {   # stage <built dll> <name> <arms subdir>
  local dll=$1 name=$2 arm=$3 old="$APP/statics/ikvm/$2" keep tmp
  if [ -f "$old" ]; then
    keep="$WJ_STATE/arms/$arm/$(sha256sum "$old" | cut -c1-12).dll"
    mkdir -p "$(dirname "$keep")"; [ -f "$keep" ] || cp -p "$old" "$keep"
    echo "kept    $(basename "$keep")  (the $name this replaces)"
  fi
  cp "$dll" "$old"
  tmp=$(mktemp -d); cp "$dll" "$tmp/$name"
  (cd "$tmp" && zip -q "$APP/statics/ikvm.zip" "$name"); rm -rf "$tmp"
  echo "staged  $(sha256sum "$old" | cut -c1-16)  $name -> statics/ikvm/ + statics/ikvm.zip"
}
stage "$DLL" IKVM.Runtime.dll ikvm-runtime
[ -z "$JDLL" ] || stage "$JDLL" IKVM.Java.dll ikvm-java
echo "now: wj deploy <marker>"
