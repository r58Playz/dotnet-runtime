#!/bin/bash
# ikvm-runtime.sh [--install] -- rebuild IKVM.Runtime only (R516's recipe: build-ikvm-local.sh's version properties, so
# the assembly identity matches the shipped CI build), and with --install stage it into ikvmcraft for deploy.sh.
#
# Staging writes BOTH copies (R514): statics/ikvm/IKVM.Runtime.dll, which publish reads, and the entry in statics/ikvm.zip,
# because `make ikvmc-bundles` re-unzips that zip over the directory. The replaced DLL is kept under
# $WJ_STATE/arms/ikvm-runtime/<sha12>.dll. After deploy, prove the change is SERVED: deploy.sh checks the served
# IKVM.Runtime.*.dll byte count; compare its sha256 against the one printed here (an env-var name in it is UTF-16 in the
# #US heap, so a plain ASCII grep for a knob name finds nothing -- R372).
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/../lib/env.sh"
SRC=$(need IKVM_SRC "the IKVM fork checkout")
BUILD=$(need IKVM_BUILD "ikvm-wasm-build: its .nuget cache restores IKVM's packages")
cd "$SRC"
export NUGET_PACKAGES=$BUILD/.nuget/packages
export GitVersion_FullSemVer=8.14.0 GitVersion_SemVer=8.14.0 GitVersion_AssemblySemVer=8.14.0.0 GitVersion_AssemblySemFileVer=8.14.0.0 GitVersion_InformationalVersion=8.14.0 GitVersion_PreReleaseLabel= GitVersion_WeightedPreReleaseNumber=0
dotnet msbuild /m /nr:false /restore /p:Configuration=Release /p:PreReleaseLabel= /p:PreReleaseNumber=0 /p:Version=8.14.0 \
  /p:AssemblyVersion=8.14.0.0 /p:InformationalVersion=8.14.0 /p:FileVersion=8.14.0.0 /p:PackageVersion=8.14.0 \
  /p:RepositoryUrl=https://github.com/ikvmnet/ikvm.git /p:PackageProjectUrl=https://github.com/ikvmnet/ikvm \
  /p:ContinuousIntegrationBuild=true /p:EnabledRuntimes=linux-x64 /p:EnabledImageRuntimes=linux-x64 \
  /p:EnabledImageBinRuntimes=linux-x64 /p:EnabledToolRuntimes=linux-x64 /p:RuntimeIdentifier=linux-x64 \
  /p:BuildNet6JreImages=false /p:EnableOSXCodeSign=false src/IKVM.Runtime/IKVM.Runtime.csproj
DLL=$SRC/src/IKVM.Runtime/bin/Release/net8.0/IKVM.Runtime.dll
echo "built   $(sha256sum "$DLL" | cut -c1-16)  $DLL"
[ "${1:-}" = "--install" ] || exit 0

APP=$(need IKVMCRAFT)
OLD=$APP/statics/ikvm/IKVM.Runtime.dll
if [ -f "$OLD" ]; then
  keep="$WJ_STATE/arms/ikvm-runtime/$(sha256sum "$OLD" | cut -c1-12).dll"
  mkdir -p "$(dirname "$keep")"; [ -f "$keep" ] || cp -p "$OLD" "$keep"
  echo "kept    $(basename "$keep")  (the DLL this replaces)"
fi
cp "$DLL" "$OLD"
tmp=$(mktemp -d); cp "$DLL" "$tmp/IKVM.Runtime.dll"
(cd "$tmp" && zip -q "$APP/statics/ikvm.zip" IKVM.Runtime.dll); rm -rf "$tmp"
echo "staged  $(sha256sum "$OLD" | cut -c1-16)  statics/ikvm/ + statics/ikvm.zip; now: wj deploy <marker>"
