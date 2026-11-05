#!/bin/bash
# One arm of a boot-crash A/B: N sequential bootchecks under one knob set, reported as crashes/valid.
#
# Two things this does that a bare bootcheck loop does not, and both were paid for:
#   - VALIDITY. A boot whose log is <1000 lines never launched the game (dev server down, seed missing).
#     Several arms have been scored on runs where nothing ran; a 0/3 from those looks like a clean arm.
#   - The crash TEXT, not just the count. If a knob changes WHICH exception is thrown, that is information
#     and a bare count throws it away.
#
# usage: arm.sh <tag> <n> [--knob k=v ...]    (logs in $ARM_OUT, default runs/<today>-arm-<tag>)
. "$(dirname "${BASH_SOURCE[0]}")/../lib/env.sh"
tag=$1; n=$2; shift 2
O=${ARM_OUT:-$(wj_rundir "arm-$tag")}
c=0; v=0
for i in $(seq 1 "$n"); do
  timeout 400 wj bootcheck --out "$O" --tag "$tag$i" "$@" >/dev/null 2>&1
  L=$O/$tag$i.log
  lines=$( [ -f "$L" ] && wc -l < "$L" || echo 0 )
  if [ "$lines" -le 1000 ]; then echo "  $tag$i: INVALID ($lines lines)"; continue; fi
  v=$((v+1))
  # WASM_JIT_IMPORT_NOT_LIVE says the resolver refused a method import because the callee was not
  # instantiated on that worker. Zero is the goal; a high count means coverage is being lost, which is a
  # different problem from a crash and would otherwise be invisible behind a PASS.
  nl=$(grep -c 'WASM_JIT_IMPORT_NOT_LIVE' "$L" 2>/dev/null || echo 0)
  # The LINKAGE family is in this pattern deliberately. It was not, and R270's IKVM CallerID regression
  # -- which aborts Fabric with `IllegalAccessError` and never reaches a "Game crashed" line -- would
  # have scored `ok` in BOTH arms, i.e. the A/B could not have seen the thing it was run to measure.
  # A gate that cannot reach the case a change introduces is not evidence about that change.
  why=$(grep -m1 -oE 'Game crashed.*|IllegalStateException[^"]*|RuntimeError[^"]*|java\.lang\.(IllegalAccessError|NoClassDefFoundError|VerifyError|LinkageError|IncompatibleClassChangeError|NoSuchMethodError|NoSuchFieldError|AbstractMethodError|ClassFormatError)[^"]*' "$L" | head -c 300)
  # 300, not 120: `IllegalAccessError: Try to access class <X> from class <Y>` was cut at `from cl`,
  # hiding the caller -- the half that actually identified the bug. Cost a ~50 min cycle (R270).
  # A truncation in a diagnostic decides which hypotheses are reachable; it is not cosmetic.
  if [ -n "$why" ]; then c=$((c+1)); echo "  $tag$i: CRASH notlive=$nl  $why"; else echo "  $tag$i: ok notlive=$nl"; fi
done
echo "$tag: crashes=$c/$v valid  [$*]"
