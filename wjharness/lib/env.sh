# Sourced by every harness shell script:
#   . "$(dirname "${BASH_SOURCE[0]}")/../lib/env.sh"
# Exports the resolved configuration (lib/paths.mjs is the one implementation: WJ_HARNESS, WJ_REPO, WJ_HOME,
# WJ_STATE, WJ_RUNS, WJ_NOTES, IKVMCRAFT, CHROME_BIN, SERVE_URL, WJ_PCPUS, WJ_PMU_CORE, ...) and defines the helpers
# below. Under `wj` the values are already exported and this costs nothing.
if [ -z "${WJ_HARNESS:-}" ] || [ -z "${WJ_HOME:-}" ]; then
  eval "$(node "$(dirname "${BASH_SOURCE[0]}")/paths.mjs" --sh)" || { echo "env.sh: lib/paths.mjs failed" >&2; return 1 2>/dev/null || exit 1; }
fi

# `wj` on PATH, so scripts (and the timeout / taskset / nice they wrap commands in) can call it by name.
case ":$PATH:" in *":$WJ_HARNESS:"*) ;; *) export PATH="$WJ_HARNESS:$PATH" ;; esac

# need KEY [why] -- the value of a configured key, or a message naming the machine file and exit 1.
need () {
  local v="${!1:-}"
  if [ -z "$v" ]; then
    echo "$1 is not configured${2:+ ($2)}: set it in ${WJ_MACHINE_ENV} (see $WJ_HARNESS/machine.env.example) or the environment" >&2
    exit 1
  fi
  printf '%s' "$v"
}

# pmu_require -- refuse unless this machine's perf event encodings are the ones the raw-code event sets were written for.
# C / A are the big- and small-core PMU names (cpu_core / cpu_atom on hybrid Intel).
pmu_require () {
  if [ "${WJ_UARCH:-}" != "glc-grt" ] && [ -z "${WJ_PMU_FORCE:-}" ]; then
    echo "refusing: this event set uses Golden Cove + Gracemont raw codes (Alder/Raptor Lake hybrid) and this CPU is" \
         "'${WJ_UARCH:-?}'. Declare an event set for it, or WJ_PMU_FORCE=1 if you know the codes match." >&2
    exit 2
  fi
  C="$WJ_PMU_CORE"; A="$WJ_PMU_ATOM"
}

# wj_setenv_args -- `--setenv=NAME` for every harness variable, for a systemd-run unit (which starts from the user
# manager's environment, not this shell's).
wj_setenv_args () {
  local v
  while read -r v; do
    case "$v" in WJ_*|IKVMCRAFT|IKVM_SRC|IKVM_BUILD|MESA_WINK|TEAVM_*|CHROMIUM_SRC|PRISM_INSTANCE|NW_TAR|D8|FNA_WASM_BUILD|DOTNET_ZIP|EMSDK|CHROME_BIN|SERVE_URL|DEV_UNIT|DEV_CMD|JAVA_HOME_*|PATH) printf -- '--setenv=%s\n' "$v" ;; esac
  done < <(compgen -e)
}

# wj_outdir DIR -- refuse an output directory inside the harness code.
wj_outdir () {
  local abs; abs=$(realpath -m "$1")
  case "$abs/" in "$WJ_HARNESS"/*) echo "refusing to write output inside the harness code ($abs); use $WJ_RUNS/..." >&2; exit 1 ;; esac
  printf '%s' "$abs"
}

# wj_rundir TAG -- runs/<YYYY-MM-DD>-<tag>, created.
wj_rundir () {
  local d="$WJ_RUNS/$(date +%F)-$1"
  [ -e "$d" ] && d="$d-$(date +%H%M%S)"
  mkdir -p "$d" && printf '%s' "$d"
}
