#!/bin/bash
# Run inside the container with the IKVM checkout at /ikvm: wj ikvm-runtime --java's IKVM.Java build.
set -euo pipefail
cd /ikvm
export NUGET_PACKAGES=/root/.nuget/packages DOTNET_CLI_TELEMETRY_OPTOUT=1
export GitVersion_FullSemVer=8.14.0 GitVersion_SemVer=8.14.0 GitVersion_AssemblySemVer=8.14.0.0 GitVersion_AssemblySemFileVer=8.14.0.0 GitVersion_InformationalVersion=8.14.0 GitVersion_PreReleaseLabel= GitVersion_WeightedPreReleaseNumber=0
PROPS=(/p:Configuration=Release /p:EnabledRuntimes=linux-x64 /p:EnabledImageRuntimes=linux-x64 /p:EnabledImageBinRuntimes=linux-x64
  /p:EnabledToolRuntimes=linux-x64 /p:RuntimeIdentifier=linux-x64 /p:BuildNet6JreImages=false)
for t in src/ikvmc/ikvmc.csproj src/ikvmstub/ikvmstub.csproj; do dotnet msbuild /t:Restore /nr:false "${PROPS[@]}" "$t" >/dev/null; done
dotnet msbuild /m /nr:false /restore "${PROPS[@]}" /p:PreReleaseLabel= /p:PreReleaseNumber=0 /p:Version=8.14.0 \
  /p:AssemblyVersion=8.14.0.0 /p:InformationalVersion=8.14.0 /p:FileVersion=8.14.0.0 /p:PackageVersion=8.14.0 \
  /p:RepositoryUrl=https://github.com/ikvmnet/ikvm.git /p:PackageProjectUrl=https://github.com/ikvmnet/ikvm \
  /p:ContinuousIntegrationBuild=true src/IKVM.Java.runtime.linux/IKVM.Java.runtime.linux.ikvmproj
ls -la artifacts/jdk25/bin/IKVM.Java.runtime.linux/Release/net8.0/*/IKVM.Java.dll artifacts/jdk25/bin/IKVM.Java.runtime.linux/Release/net8.0/IKVM.Java.dll 2>/dev/null || true
