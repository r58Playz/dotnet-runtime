#!/usr/bin/env bash
# The pinned TeaVM (9be5eca57, the last with the linear-memory wasm backend; wj bench/teavm-cli builds it):
#   wj bench/teavm -t wasm -O 3 -p <classpath entry> [-p ...] -d <outdir> -f <name>.wasm <main class>
# Runs org.teavm.cli.TeaVMRunner on JDK 21 with TEAVM_JVM_OPTS (default -Xmx8g). The CLI jar itself goes first on
# -p: at this commit the runner reads classes from the -p FILES (tool.setClassPath), not from its own class loader, so
# without it TeaVM's runtime and classlib classes are missing (an NPE in WasmTarget.contributeDependencies).
set -euo pipefail
. "$(dirname "$0")/../lib/env.sh"
D="$WJ_STATE/arms/teavm-cli"
[ -f "$D/teavm-cli-9be5eca57.jar" ] && [ -f "$D/commons-cli-1.10.0.jar" ] || { echo "run wj bench/teavm-cli first" >&2; exit 1; }
exec "$(need JAVA_HOME_21)/bin/java" ${TEAVM_JVM_OPTS:--Xmx8g} -cp "$D/teavm-cli-9be5eca57.jar:$D/commons-cli-1.10.0.jar" \
  org.teavm.cli.TeaVMRunner -p "$D/teavm-cli-9be5eca57.jar" -p "$D/teavm-extra.jar" "$@"
